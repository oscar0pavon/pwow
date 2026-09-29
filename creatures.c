#include "creatures.h"

#include <engine/array.h>
#include <engine/files.h>
#include <engine/images.h>
#include <engine/macros.h>
#include <engine/model.h>
#include <engine/renderer/descriptor_set.h>
#include <engine/renderer/draw.h>
#include <engine/renderer/shaders.h>
#include <engine/renderer/uniform_buffer.h>
#include <engine/wowauth/wowdbc.h>

#include <ctype.h>
#include <string.h>

#define CREATURE_DISPLAY_DBC_PATH "data/dbc/CreatureDisplayInfo.dbc"
#define CREATURE_MODEL_DBC_PATH "data/dbc/CreatureModelData.dbc"

#define CREATURE_DISPLAY_FIELD_ID 0
#define CREATURE_DISPLAY_FIELD_MODEL_ID 1
#define CREATURE_DISPLAY_FIELD_EXTENDED 3
#define CREATURE_DISPLAY_FIELD_TEXTURE0 6

#define CREATURE_MODEL_FIELD_ID 0
#define CREATURE_MODEL_FIELD_NAME 2

static PShader creature_shader;

//---------------------------------------------------------------------------
//display id -> what to load, cached. tools/resolve_creatures.c re-derives
//the same DBC rule offline, for prepare_creatures.sh; this is the runtime
//half, same split as adt2wot's tile math and terrain_world.c's own
//---------------------------------------------------------------------------

typedef struct ResolvedDisplay {
  u32 display_id;
  bool simple;
  char glb_path[256];
  char texture_path[256]; //empty: use the glb's own baked-in texture
} ResolvedDisplay;

#define RESOLVED_DISPLAYS_MAX 64
static ResolvedDisplay resolved_displays[RESOLVED_DISPLAYS_MAX];
static int resolved_display_count;

//Creature\Tallstrider\TallStrider.mdx -> creature/tallstrider/tallstrider.m2,
//the same normalisation tools/adt2wot.c's resolve_model() and
//tools/resolve_creatures.c both already apply to a model path
static void normalise_model_path(char *name) {
  for (char *c = name; *c; c++)
    *c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);

  size_t length = strlen(name);
  if (length > 4 && (strcmp(name + length - 4, ".mdx") == 0 ||
                     strcmp(name + length - 4, ".mdl") == 0))
    strcpy(name + length - 4, ".m2");
}

static void lowercase(char *name) {
  for (char *c = name; *c; c++)
    *c = (char)tolower((unsigned char)*c);
}

static ResolvedDisplay *resolve_display(u32 display_id) {
  for (int i = 0; i < resolved_display_count; i++)
    if (resolved_displays[i].display_id == display_id)
      return &resolved_displays[i];

  if (resolved_display_count >= RESOLVED_DISPLAYS_MAX)
    return NULL;

  ResolvedDisplay *out = &resolved_displays[resolved_display_count++];
  ZERO(*out);
  out->display_id = display_id;

  PWowDBC display_dbc, model_dbc;
  if (!pe_wowdbc_load(CREATURE_DISPLAY_DBC_PATH, &display_dbc))
    return out;
  if (!pe_wowdbc_load(CREATURE_MODEL_DBC_PATH, &model_dbc)) {
    pe_wowdbc_free(&display_dbc);
    return out;
  }

  for (u32 r = 0; r < display_dbc.record_count; r++) {
    if (pe_wowdbc_get_u32(&display_dbc, r, CREATURE_DISPLAY_FIELD_ID) != display_id)
      continue;
    //nonzero: a humanoid, driven by CreatureDisplayInfoExtra's race/gender/
    //skin/face/hair/equipment - the player-character pipeline, not this one
    if (pe_wowdbc_get_u32(&display_dbc, r, CREATURE_DISPLAY_FIELD_EXTENDED) != 0)
      break;

    u32 model_id =
        pe_wowdbc_get_u32(&display_dbc, r, CREATURE_DISPLAY_FIELD_MODEL_ID);
    u32 model_record = model_dbc.record_count;
    for (u32 m = 0; m < model_dbc.record_count; m++) {
      if (pe_wowdbc_get_u32(&model_dbc, m, CREATURE_MODEL_FIELD_ID) == model_id) {
        model_record = m;
        break;
      }
    }
    if (model_record == model_dbc.record_count)
      break;

    char model_path[256];
    snprintf(model_path, sizeof(model_path), "%s",
            pe_wowdbc_get_string(&model_dbc, model_record,
                                 CREATURE_MODEL_FIELD_NAME));
    if (model_path[0] == '\0')
      break;
    normalise_model_path(model_path);

    size_t model_len = strlen(model_path);
    snprintf(out->glb_path, sizeof(out->glb_path), "data/%.*s.glb",
             (int)(model_len - 3), model_path);

    char variation[192];
    snprintf(variation, sizeof(variation), "%s",
            pe_wowdbc_get_string(&display_dbc, r,
                                 CREATURE_DISPLAY_FIELD_TEXTURE0));
    if (variation[0] != '\0') {
      lowercase(variation);

      char dir[256];
      snprintf(dir, sizeof(dir), "%s", model_path);
      char *slash = strrchr(dir, '/');
      if (slash)
        *slash = '\0';
      else
        dir[0] = '\0';

      snprintf(out->texture_path, sizeof(out->texture_path), "data/%s%s%s.png",
               dir, dir[0] ? "/" : "", variation);
    }

    out->simple = true;
    break;
  }

  pe_wowdbc_free(&display_dbc);
  pe_wowdbc_free(&model_dbc);
  return out;
}

//---------------------------------------------------------------------------
//one loaded PModel per unique species (glb path), shared by every instance
//of it via pe_vk_model_instance()
//---------------------------------------------------------------------------

typedef struct CreatureTemplate {
  char glb_path[256];
  PModel model;
} CreatureTemplate;

#define CREATURE_TEMPLATES_MAX 16
static CreatureTemplate creature_templates[CREATURE_TEMPLATES_MAX];
static int creature_template_count;

static PModel *find_or_load_template(const ResolvedDisplay *resolved) {
  for (int i = 0; i < creature_template_count; i++)
    if (strcmp(creature_templates[i].glb_path, resolved->glb_path) == 0)
      return &creature_templates[i].model;

  if (creature_template_count >= CREATURE_TEMPLATES_MAX)
    return NULL;

  CreatureTemplate *t = &creature_templates[creature_template_count++];
  snprintf(t->glb_path, sizeof(t->glb_path), "%s", resolved->glb_path);

  //pe_vk_load_model() already builds the plain (uniform buffer + texture)
  //descriptor pool/sets - the same layout pe_vk_model_instance() copies
  //from, so a static creature mesh needs nothing skinned.vert's player path
  //does (no joint storage buffer, no second descriptor-set remake)
  pe_vk_load_model(&t->model, t->glb_path);
  t->model.shader = creature_shader;

  //present only when the model's own texture slot is a replaceable type
  //m22gltf leaves unresolved (model_texture(), m22gltf.c) - otherwise the
  //glb's own baked-in material texture is already correct
  if (resolved->texture_path[0] != '\0')
    pe_load_texture(resolved->texture_path, &t->model.texture);

  return &t->model;
}

//---------------------------------------------------------------------------
//one GPU instance per currently-tracked simple creature
//---------------------------------------------------------------------------

typedef struct CreatureInstance {
  u64 guid;
  PModel model;
  bool used;
} CreatureInstance;

#define CREATURE_INSTANCES_MAX 32
static CreatureInstance creature_instances[CREATURE_INSTANCES_MAX];

//only what an instance actually owns - never pe_clean_model() (engine/
//model.c), which unconditionally frees vertex_buffer.memory/index_buffer.
//memory. pe_vk_model_instance() shares those with its template (and every
//other instance of the same species) by design, so that would free
//geometry still in use elsewhere
static void release_creature_instance(PModel *model) {
  for (int i = 0; i < model->uniform_buffers_memory.count; i++) {
    VkDeviceMemory *memory = array_get(&model->uniform_buffers_memory, i);
    vkFreeMemory(vk_device, *memory, NULL);
  }
  vkDestroyDescriptorPool(vk_device, model->descriptor_pool, NULL);
}

void creatures_init(void) {
  PCreateShaderInfo shader_info;
  ZERO(shader_info);
  shader_info.out_shader = &creature_shader;
  shader_info.vertex_path = file_diffuse_vert_spv;
  shader_info.fragment_path = file_diffuse_frag_spv;
  //pe_vk_pipeline_layout is declared (descriptor_set.h) but never actually
  //created anywhere in the engine - nothing has ever used it, since
  //pe_vk_load_model()'s own plain descriptor sets are always either thrown
  //away and rebuilt skinned (the player) or never paired with a real
  //pipeline before this. pe_vk_pipeline_layout_with_descriptors is the one
  //that's actually wired up (vulkan.c) against the same
  //pe_vk_descriptor_set_layout pe_vk_load_model() uses - confirmed already
  //working via gui.c's button quads, which is why it's used here instead
  shader_info.layout = pe_vk_pipeline_layout_with_descriptors;
  //vertex_input left NULL: the engine's default (position + uv) is exactly
  //what the plain diffuse shader reads, same as any other untextured/
  //textured static model
  pe_vk_create_shader(&shader_info);
}

void creatures_sync(const PWowObjectState *npc_state) {
  bool touched[CREATURE_INSTANCES_MAX];
  memset(touched, 0, sizeof(touched));

  for (int i = 0; i < npc_state->count; i++) {
    const PWowCreature *creature = &npc_state->creatures[i];
    ResolvedDisplay *resolved = resolve_display(creature->display_id);
    if (!resolved || !resolved->simple)
      continue;

    int slot = -1;
    for (int s = 0; s < CREATURE_INSTANCES_MAX; s++) {
      if (creature_instances[s].used && creature_instances[s].guid == creature->guid) {
        slot = s;
        break;
      }
    }

    if (slot < 0) {
      for (int s = 0; s < CREATURE_INSTANCES_MAX; s++) {
        if (!creature_instances[s].used) {
          slot = s;
          break;
        }
      }
      if (slot < 0)
        continue; //pool full - this creature waits for a slot to free up

      PModel *template = find_or_load_template(resolved);
      if (!template)
        continue;

      pe_vk_model_instance(&creature_instances[slot].model, template);
      creature_instances[slot].guid = creature->guid;
      creature_instances[slot].used = true;
    }

    touched[slot] = true;

    //translate, turn to face (o is already radians off the wire, unlike
    //pwow's own live-character facing which is tracked in degrees), then
    //undo m22gltf's -90-about-X Z-up-to-Y-up export bake - same
    //composition player_place() uses, and for the same reason: the facing
    //turn has to happen before the up-axis fix, not after
    PModel *instance = &creature_instances[slot].model;
    glm_mat4_identity(instance->model_mat);
    glm_translate(instance->model_mat, (vec3){creature->x, creature->y, creature->z});
    glm_rotate(instance->model_mat, creature->o, (vec3){0, 0, 1});
    glm_rotate(instance->model_mat, glm_rad(90.0f), (vec3){1, 0, 0});
    glm_vec3_copy((vec3){creature->x, creature->y, creature->z}, instance->position);
  }

  for (int s = 0; s < CREATURE_INSTANCES_MAX; s++) {
    if (creature_instances[s].used && !touched[s]) {
      release_creature_instance(&creature_instances[s].model);
      creature_instances[s].used = false;
    }
  }
}

void creatures_draw(VkCommandBuffer *command, uint32_t image_index,
                    mat4 view, mat4 projection) {
  for (int s = 0; s < CREATURE_INSTANCES_MAX; s++) {
    if (!creature_instances[s].used)
      continue;

    PModel *instance = &creature_instances[s].model;
    PUniformBufferObject *ubo = &instance->uniform_buffer_object;
    glm_mat4_copy(instance->model_mat, ubo->model);
    glm_mat4_copy(view, ubo->view);
    glm_mat4_copy(projection, ubo->projection);

    //same guessed-sun-direction light placement player_draw() uses
    vec3 light_position;
    glm_vec3_copy(instance->model_mat[3], light_position);
    glm_vec3_muladds((vec3){0.4f, -0.3f, 0.8f}, 5000.0f, light_position);
    glm_vec4(light_position, 1, ubo->light_position);

    pe_vk_send_uniform_buffer(instance, image_index);

    PDrawModelCommand draw;
    ZERO(draw);
    draw.model = instance;
    draw.layout = pe_vk_pipeline_layout_with_descriptors;
    draw.command_buffer = *command;
    draw.image_index = image_index;
    pe_vk_draw_model(&draw);
  }
}
