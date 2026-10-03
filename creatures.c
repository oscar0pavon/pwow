#include "creatures.h"

#include <engine/animation/animation.h>
#include <engine/array.h>
#include <engine/files.h>
#include <engine/images.h>
#include <engine/macros.h>
#include <engine/model.h>
#include <engine/renderer/descriptor_set.h>
#include <engine/renderer/draw.h>
#include <engine/renderer/pipeline.h>
#include <engine/renderer/shaders.h>
#include <engine/renderer/uniform_buffer.h>
#include <engine/renderer/vk_images.h>
#include <engine/renderer/vk_vertex.h>
#include <engine/renderer/vulkan.h>
#include <engine/skeletal.h>
#include <engine/vertex.h>
#include <wowauth/wowdbc.h>

#include "equipment.h"

#include <ctype.h>
#include <float.h>
#include <string.h>

#define CREATURE_DISPLAY_DBC_PATH "data/dbc/CreatureDisplayInfo.dbc"
#define CREATURE_MODEL_DBC_PATH "data/dbc/CreatureModelData.dbc"

#define CREATURE_DISPLAY_FIELD_ID 0
#define CREATURE_DISPLAY_FIELD_MODEL_ID 1
#define CREATURE_DISPLAY_FIELD_EXTENDED 3
#define CREATURE_DISPLAY_FIELD_TEXTURE0 6

#define CREATURE_EXTRA_DBC_PATH "data/dbc/CreatureDisplayInfoExtra.dbc"

#define CREATURE_EXTRA_FIELD_ID 0
#define CREATURE_EXTRA_FIELD_RACE 1
#define CREATURE_EXTRA_FIELD_GENDER 2
#define CREATURE_EXTRA_FIELD_SKIN 3
#define CREATURE_EXTRA_FIELD_FACE 4
#define CREATURE_EXTRA_FIELD_HAIR_STYLE 5
#define CREATURE_EXTRA_FIELD_HAIR_COLOR 6

#define TAUREN_RACE_ID 6


#define CREATURE_MODEL_FIELD_ID 0
#define CREATURE_MODEL_FIELD_NAME 2

static PShader creature_shader;
static PShader humanoid_shader;

//how far each body's lowest vertex sits below its root bone in the Stand pose,
//measured by skinning the mesh through the clip's first keyframe
static const float HUMANOID_FOOT_OFFSETS[] = {
    [SEX_MALE] = 0.011f,
    [SEX_FEMALE] = 0.004f,
};

static const char *const HUMANOID_MODEL_PATHS[] = {
    [SEX_MALE] = "data/character/tauren/male/taurenmale.glb",
    [SEX_FEMALE] = "data/character/tauren/female/taurenfemale.glb",
};

//the equipment columns of CreatureDisplayInfoExtra, each an ItemDisplayInfo
//id (0 for an empty slot), with the InventoryType that slot stands for.
//shoulders (field 9) are left out: no geoset or texture rule uses them yet
static const struct {
  u32 field, inventory_type;
} EXTRA_EQUIPMENT_SLOTS[] = {
    {8, INVTYPE_HEAD},   {10, INVTYPE_BODY},  {11, INVTYPE_CHEST},
    {12, INVTYPE_WAIST}, {13, INVTYPE_LEGS},  {14, INVTYPE_FEET},
    {15, INVTYPE_WRISTS}, {16, INVTYPE_HANDS}, {17, INVTYPE_TABARD},
};
#define EXTRA_EQUIPMENT_SLOT_COUNT \
  (sizeof(EXTRA_EQUIPMENT_SLOTS) / sizeof(EXTRA_EQUIPMENT_SLOTS[0]))

//---------------------------------------------------------------------------
//display id -> what to load, cached. tools/resolve_creatures.c re-derives
//the same DBC rule offline, for prepare_creatures.sh; this is the runtime
//half, same split as adt2wot's tile math and terrain_world.c's own
//---------------------------------------------------------------------------

typedef struct ResolvedDisplay {
  u32 display_id;
  bool simple;
  bool humanoid;
  PAppearance look;
  PEquippedItem items[EXTRA_EQUIPMENT_SLOT_COUNT];
  int item_count;
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

//a humanoid is only drawn when CreatureDisplayInfoExtra names a Tauren, the
//one race converted so far; any other race stays unresolved
static void resolve_humanoid(ResolvedDisplay *out, u32 extra_id) {
  PWowDBC extra_dbc;
  if (!pe_wowdbc_load(CREATURE_EXTRA_DBC_PATH, &extra_dbc))
    return;

  for (u32 r = 0; r < extra_dbc.record_count; r++) {
    if (pe_wowdbc_get_u32(&extra_dbc, r, CREATURE_EXTRA_FIELD_ID) != extra_id)
      continue;
    u32 sex = pe_wowdbc_get_u32(&extra_dbc, r, CREATURE_EXTRA_FIELD_GENDER);
    if (pe_wowdbc_get_u32(&extra_dbc, r, CREATURE_EXTRA_FIELD_RACE) != TAUREN_RACE_ID ||
        (sex != SEX_MALE && sex != SEX_FEMALE))
      break;

    out->look.sex = sex;
    out->look.skin = pe_wowdbc_get_u32(&extra_dbc, r, CREATURE_EXTRA_FIELD_SKIN);
    out->look.face = pe_wowdbc_get_u32(&extra_dbc, r, CREATURE_EXTRA_FIELD_FACE);
    out->look.hair_style =
        pe_wowdbc_get_u32(&extra_dbc, r, CREATURE_EXTRA_FIELD_HAIR_STYLE);
    out->look.hair_color =
        pe_wowdbc_get_u32(&extra_dbc, r, CREATURE_EXTRA_FIELD_HAIR_COLOR);

    for (size_t slot = 0; slot < EXTRA_EQUIPMENT_SLOT_COUNT; slot++) {
      u32 display_id =
          pe_wowdbc_get_u32(&extra_dbc, r, EXTRA_EQUIPMENT_SLOTS[slot].field);
      PEquippedItem *item = &out->items[out->item_count];
      if (display_id == 0 || !resolve_item_display_info(display_id, &item->display))
        continue;
      item->inventory_type = EXTRA_EQUIPMENT_SLOTS[slot].inventory_type;
      out->item_count++;
    }
    out->humanoid = true;
    break;
  }

  pe_wowdbc_free(&extra_dbc);
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
    u32 extra_id = pe_wowdbc_get_u32(&display_dbc, r, CREATURE_DISPLAY_FIELD_EXTENDED);
    if (extra_id != 0) {
      resolve_humanoid(out, extra_id);
      break;
    }

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
//one loaded PModel+PSkin per unique species (glb path), shared by every
//instance of it via pe_vk_model_instance_skinned()
//---------------------------------------------------------------------------

//the three clips an instance switches between: standing still, and moving
//per PWowCreature.moving/walking (wowobject.h - walking is the real
//PRE_WOTLK_RUNMODE bit off the last SMSG_MONSTER_MOVE's spline flags, not a
//guess). resolved once per species in find_or_load_template(), not per
//creature, same as the game's own animation ids (4 Walk, 5 Run) are shared
//by every creature of a kind
#define CREATURE_IDLE_ANIMATION "Stand"
#define CREATURE_WALK_ANIMATION "Walk"
#define CREATURE_RUN_ANIMATION "Run"

typedef struct ClipSet {
  char idle[48];
  char walk[48];
  char run[48];
} ClipSet;

typedef struct CreatureTemplate {
  char glb_path[256];
  PModel model;
  //the species' own mesh, joint topology and animation clips - read-only
  //once loaded. never drawn or played directly and never shared with an
  //instance's own skin; pe_vk_skin_instance() copies out of this per
  //instance (see CreatureInstance below), and idle/walk/run_animation name
  //the clips each instance switches between
  PSkin skin;
  ClipSet clips;
  float foot_offset;
} CreatureTemplate;

#define CREATURE_TEMPLATES_MAX 16
static CreatureTemplate creature_templates[CREATURE_TEMPLATES_MAX];
static int creature_template_count;

//SMSG_UPDATE_OBJECT's x/y/z is the creature's feet, standing on the ground -
//same as the player's, which player_load() (main.c) corrects for with a
//constant measured against taurenmale.glb's Stand pose. A creature template
//is drawn unposed (nothing here plays an animation - see creatures.h), so
//its own bind-pose mesh *is* what's on screen, and the lowest vertex of that
//mesh is exactly how far its root sits above its own feet: no forward-
//kinematics simulation needed, unlike the player's animated case. m22gltf
//writes no accessor min/max (nothing here reads one), so it's found by
//scanning the loaded vertices directly rather than trusting glTF bounds
//that were never written. The player's own +90-about-X axis fix (m22gltf
//bakes -90 about X on export) carries a raw glTF Y straight into world Z
//unchanged - same derivation as PLAYER_FOOT_OFFSET's comment - so the
//lowest vertex's Y is the offset needed, unmodified
static float compute_foot_offset(PModel *model) {
  float min_y = FLT_MAX;
  for (int i = 0; i < model->vertex_array.count; i++) {
    PVertex *vertex = array_get(&model->vertex_array, i);
    if (vertex->position[1] < min_y)
      min_y = vertex->position[1];
  }
  return model->vertex_array.count > 0 ? -min_y : 0.0f;
}

static bool has_animation(PSkin *skin, const char *name) {
  for (int i = 0; i < skin->animations.count; i++) {
    PAnimation *animation = array_get(&skin->animations, i);
    if (strcmp(animation->name, name) == 0)
      return true;
  }
  return false;
}

//not every species has a clip literally named "Stand" (confirmed live: one
//did not, and silently rendered nothing, before the pe_anim_nodes_update()
//seed existed) - fall back to whatever its first clip actually is instead of
//leaving every instance static. the same idea for each locomotion clip:
//prefer the one the wire asked for, fall back to the other gait if that is
//all the species has, and to idle if it has neither - still moving on
//screen, just not visibly walking or running, rather than a T-pose
static void resolve_clips(PSkin *skin, ClipSet *clips) {
  const char *idle = CREATURE_IDLE_ANIMATION;
  if (skin->animations.count > 0 && !has_animation(skin, idle)) {
    PAnimation *first = array_get(&skin->animations, 0);
    idle = first->name;
  }
  snprintf(clips->idle, sizeof(clips->idle), "%s", idle);

  const char *walk = clips->idle;
  if (has_animation(skin, CREATURE_WALK_ANIMATION))
    walk = CREATURE_WALK_ANIMATION;
  else if (has_animation(skin, CREATURE_RUN_ANIMATION))
    walk = CREATURE_RUN_ANIMATION;
  snprintf(clips->walk, sizeof(clips->walk), "%s", walk);

  const char *run = clips->idle;
  if (has_animation(skin, CREATURE_RUN_ANIMATION))
    run = CREATURE_RUN_ANIMATION;
  else if (has_animation(skin, CREATURE_WALK_ANIMATION))
    run = CREATURE_WALK_ANIMATION;
  snprintf(clips->run, sizeof(clips->run), "%s", run);
}

static CreatureTemplate *find_or_load_template(const ResolvedDisplay *resolved) {
  for (int i = 0; i < creature_template_count; i++)
    if (strcmp(creature_templates[i].glb_path, resolved->glb_path) == 0)
      return &creature_templates[i];

  if (creature_template_count >= CREATURE_TEMPLATES_MAX)
    return NULL;

  CreatureTemplate *t = &creature_templates[creature_template_count++];
  snprintf(t->glb_path, sizeof(t->glb_path), "%s", resolved->glb_path);

  //pe_vk_load_skin() is pe_vk_load_model() plus, when the glTF carries one
  //(every creature .m2 does - see m22gltf.c), the joint hierarchy and
  //animation clips into t->skin; t->model gets the mesh exactly like the
  //plain loader would. Both still only build a descriptor pool/sets against
  //pe_vk_descriptor_set_layout - the uniform-buffer-only layout - which
  //neither the texture skinned.vert's joint storage buffer nor
  //diffuse_frag's sampler at binding 1 are declared in. Same fix
  //player_load() applies: throw those plain sets away and remake them
  //against the skinned layout, which does declare both, then point the
  //storage-buffer binding at this species' own joint matrices
  pe_vk_load_skin(&t->skin, &t->model, t->glb_path);
  t->model.shader = creature_shader;
  t->foot_offset = compute_foot_offset(&t->model);

  //present only when the model's own texture slot is a replaceable type
  //m22gltf leaves unresolved (model_texture(), m22gltf.c) - a display's own
  //texture variation, when there is one, overrides the glb's baked-in look
  if (resolved->texture_path[0] != '\0')
    pe_load_texture(resolved->texture_path, &t->model.texture);

  pe_vk_create_descriptor_sets(&t->model, pe_vk_descriptor_set_layout_skinned,
                               &main_render_target);
  pe_vk_skin_create_storage_buffers(&t->skin);
  pe_vk_descriptor_skinned_update(&t->model, &t->skin, &main_render_target);

  //skinned.vert's skin_mat is a weighted sum of joint matrices with no safe
  //zero fallback - a joint this never wrote into (node_uniform starts
  //zeroed, same as the rest of a freshly zero-initialized static
  //CreatureTemplate) collapses every vertex weighted to it to the origin,
  //which is what an un-posed skin actually looks like: not the bind pose,
  //a point. Seed a real pose immediately, from whatever rest values
  //pe_node_load() already put in each joint - the same values play_animation
  //would overwrite once a clip actually starts touching them - so a species
  //that ends up playing no clip at all still renders in its bind pose
  //instead of vanishing
  if (t->skin.joints.count > 0)
    pe_anim_nodes_update(&t->skin);

  resolve_clips(&t->skin, &t->clips);

  return t;
}

//---------------------------------------------------------------------------
//one GPU instance per currently-tracked simple creature
//---------------------------------------------------------------------------

typedef struct CreatureInstance {
  u64 guid;
  PModel model;
  //this instance's own posable joints, animation clock and joint-matrix
  //storage buffer - pe_vk_skin_instance() copies the species' template.skin
  //into this rather than sharing it, so two creatures of the same species
  //can be mid-idle at different phases (and, once something drives more
  //than one clip per species, in different states entirely) instead of
  //being locked to one shared pose
  PSkin skin;
  ClipSet clips;
  float foot_offset;
  //a humanoid has its own index buffer and composited texture, which
  //equipment rewrites per creature; a simple creature shares its template's
  bool dressed;
  bool used;
} CreatureInstance;

#define CREATURE_INSTANCES_MAX 32
static CreatureInstance creature_instances[CREATURE_INSTANCES_MAX];

//only what an instance actually owns. every instance shares its vertex
//buffer with its template, and a simple creature its index buffer and texture
//too, so pe_clean_model(), which frees the buffers and the shader, is never
//an option; a humanoid owns its index buffer and texture and gives those back. skin's shader_storage_buffers_memory is the instance's own
//(pe_vk_skin_instance()/pe_vk_load_skin() + pe_vk_skin_create_storage_
//buffers()); the joints/animations arrays are arena memory with no free,
//same as everything else CPU-side here
static void release_creature_instance(CreatureInstance *inst) {
  PModel *model = &inst->model;
  vkDeviceWaitIdle(vk_device);

  for (int i = 0; i < model->uniform_buffers_memory.count; i++) {
    VkDeviceMemory *memory = array_get(&model->uniform_buffers_memory, i);
    vkFreeMemory(vk_device, *memory, NULL);
  }
  vkDestroyDescriptorPool(vk_device, model->descriptor_pool, NULL);

  for (int i = 0; i < inst->skin.shader_storage_buffers_memory.count; i++) {
    VkDeviceMemory *memory =
        array_get(&inst->skin.shader_storage_buffers_memory, i);
    vkFreeMemory(vk_device, *memory, NULL);
  }

  if (!inst->dressed)
    return;

  if (model->has_extra_texture) {
    vkDestroyDescriptorPool(vk_device, model->extra_descriptor_pool, NULL);
    pe_vk_clean_image(&model->extra_texture);
  }
  pe_vk_clean_image(&model->texture);
  vkFreeMemory(vk_device, model->index_buffer.memory, NULL);
}

static void create_creature_shader(PShader *out_shader, const char *fragment_path) {
  PCreateShaderInfo shader_info;
  ZERO(shader_info);
  shader_info.out_shader = out_shader;
  //same pairing player_load() uses: file_skinned_spv does the joint-matrix
  //skinning, the fragment shader samples a texture
  shader_info.vertex_path = file_skinned_spv;
  shader_info.fragment_path = fragment_path;
  shader_info.layout = pe_vk_pipeline_layout_skinned;

  //vertex_input left NULL gets the engine's default (position + uv only,
  //shaders.c), but skinned.vert also declares color (location 1, unused -
  //dead input), normal (location 2, feeds the diffuse term), joint
  //(location 4) and weight (location 5) - the same gap player_load() hit
  //(VUID-VkGraphicsPipelineCreateInfo-Input-07904) and had to build a real
  //vertex input for
  PVertexAtrributes creature_attributes;
  ZERO(creature_attributes);
  creature_attributes.has_attributes = true;
  creature_attributes.position = true;
  creature_attributes.color = true;
  creature_attributes.normal = true;
  creature_attributes.uv = true;
  creature_attributes.joint = true;
  creature_attributes.weight = true;
  VkPipelineVertexInputStateCreateInfo creature_vertex_input =
      pe_vk_pipeline_get_default_vertex_input(&creature_attributes);
  shader_info.vertex_input = &creature_vertex_input;

  pe_vk_create_shader(&shader_info);
}

void creatures_init(void) {
  create_creature_shader(&creature_shader, file_diffuse_frag_spv);
  //a humanoid's hair, mane and cloth are alpha-cut, as the player's are
  create_creature_shader(&humanoid_shader, file_diffuse_cutout_frag_spv);
}

//seeds a real pose immediately: skinned.vert's weighted sum of joint matrices
//has no safe zero fallback, and a fresh storage buffer starts zeroed
static void seed_pose(CreatureInstance *inst) {
  if (inst->skin.joints.count > 0)
    pe_anim_nodes_update(&inst->skin);
}

static bool create_simple_instance(CreatureInstance *inst,
                                   const ResolvedDisplay *resolved) {
  CreatureTemplate *template = find_or_load_template(resolved);
  if (!template)
    return false;

  pe_vk_skin_instance(&inst->skin, &template->skin);
  pe_vk_skin_create_storage_buffers(&inst->skin);
  pe_vk_model_instance_skinned(&inst->model, &template->model, &inst->skin);
  seed_pose(inst);

  inst->clips = template->clips;
  inst->foot_offset = template->foot_offset;
  inst->dressed = false;
  return true;
}

//one loaded body per sex, shared by every humanoid of it: only the mesh,
//joint topology and clips are read from it, never drawn or dressed itself
typedef struct HumanoidTemplate {
  bool loaded;
  PModel model;
  PSkin skin;
  ClipSet clips;
} HumanoidTemplate;

static HumanoidTemplate humanoid_templates[SEX_FEMALE + 1];

static HumanoidTemplate *find_or_load_humanoid_template(u8 sex) {
  HumanoidTemplate *template = &humanoid_templates[sex];
  if (template->loaded)
    return template;

  pe_vk_load_skin(&template->skin, &template->model, HUMANOID_MODEL_PATHS[sex]);
  template->model.shader = humanoid_shader;

  //an instance copies this texture and binds it into its descriptor sets as
  //it is made, so the template needs a real one, though no instance keeps it
  PAppearance default_look = {.sex = sex};
  char skin_path[512];
  resolve_tauren_skin_path(&default_look, skin_path, sizeof(skin_path));
  pe_load_texture(skin_path, &template->model.texture);

  resolve_clips(&template->skin, &template->clips);
  template->loaded = true;
  return template;
}

//apply_equipment_geosets() replaces the index buffer it finds and destroys
//the old one, which on an instance is its template's: copy it first, so what
//gets destroyed is the instance's own
static void give_own_index_buffer(PModel *model) {
  model->index_buffer = pe_vk_create_buffer(model->index_array.bytes_size,
                                            model->index_array.data,
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

//an instance of its sex's body, dressed from its CreatureDisplayInfoExtra
//row: equipment rewrites a model's index buffer and texture in place, so
//those two are the instance's own, loaded before they are replaced since
//apply_equipment_texture() destroys the texture it finds
static bool create_humanoid_instance(CreatureInstance *inst,
                                     const ResolvedDisplay *resolved) {
  HumanoidTemplate *template = find_or_load_humanoid_template(resolved->look.sex);

  pe_vk_skin_instance(&inst->skin, &template->skin);
  pe_vk_skin_create_storage_buffers(&inst->skin);
  pe_vk_model_instance_skinned(&inst->model, &template->model, &inst->skin);

  PBodyLayers body;
  resolve_tauren_body(&resolved->look, &body);
  pe_load_texture(body.skin, &inst->model.texture);
  give_own_index_buffer(&inst->model);

  apply_equipment_geosets(&inst->model, resolved->items, resolved->item_count);
  apply_equipment_texture(&inst->model, &inst->skin, &body, resolved->items,
                          resolved->item_count);
  seed_pose(inst);

  inst->clips = template->clips;
  inst->foot_offset = HUMANOID_FOOT_OFFSETS[resolved->look.sex];
  inst->dressed = true;
  return true;
}

void creatures_sync(const PWowObjectState *npc_state) {
  bool touched[CREATURE_INSTANCES_MAX];
  memset(touched, 0, sizeof(touched));

  for (int i = 0; i < npc_state->count; i++) {
    const PWowCreature *creature = &npc_state->creatures[i];
    ResolvedDisplay *resolved = resolve_display(creature->display_id);
    if (!resolved || !(resolved->simple || resolved->humanoid))
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

      CreatureInstance *inst = &creature_instances[slot];
      bool created = resolved->humanoid
                         ? create_humanoid_instance(inst, resolved)
                         : create_simple_instance(inst, resolved);
      if (!created)
        continue;

      inst->guid = creature->guid;
      inst->used = true;
    }

    touched[slot] = true;

    //switched every frame, not just on change - play_animation_by_name()
    //already no-ops when this skin is already playing the clip it's asked
    //for (animation.c), so re-asking for the same state every frame does
    //not restart the clip from frame 0. creature->walking is the real
    //PRE_WOTLK_RUNMODE bit off the wire (wowobject.h), not a guess
    CreatureInstance *inst = &creature_instances[slot];
    if (inst->skin.animations.count > 0) {
      const char *target = inst->clips.idle;
      if (creature->moving)
        target = creature->walking ? inst->clips.walk : inst->clips.run;
      play_animation_by_name(&inst->skin, target, true);
    }

    //translate, turn to face (o is radians in the game's axes, X north and Y
    //west, where pwow's world has Y east: the mirror reverses which way a
    //turn goes, so the angle is negated here), then
    //undo m22gltf's -90-about-X Z-up-to-Y-up export bake - same
    //composition player_place() uses, and for the same reason: the facing
    //turn has to happen before the up-axis fix, not after. z is nudged up
    //by foot_offset, the model's own bind-pose lowest vertex, so the wire's
    //feet position isn't mistaken for the model root and the creature does
    //not float or sink - see compute_foot_offset() above
    PModel *instance = &creature_instances[slot].model;
    float render_z = creature->z + creature_instances[slot].foot_offset;
    glm_mat4_identity(instance->model_mat);
    glm_translate(instance->model_mat, (vec3){creature->x, creature->y, render_z});
    glm_rotate(instance->model_mat, -creature->o, (vec3){0, 0, 1});
    glm_rotate(instance->model_mat, glm_rad(90.0f), (vec3){1, 0, 0});
    glm_vec3_copy((vec3){creature->x, creature->y, render_z}, instance->position);
  }

  for (int s = 0; s < CREATURE_INSTANCES_MAX; s++) {
    if (creature_instances[s].used && !touched[s]) {
      release_creature_instance(&creature_instances[s]);
      creature_instances[s].used = false;
    }
  }
}

void creatures_draw(VkCommandBuffer *command, uint32_t image_index,
                    mat4 view, mat4 projection) {
  for (int s = 0; s < CREATURE_INSTANCES_MAX; s++) {
    if (!creature_instances[s].used)
      continue;

    //this instance's own joint matrices, computed for this frame by
    //play_animation_list() (main.c) off its own animation clock - one
    //upload per instance, not per species, now that each has its own
    //storage buffer
    pe_vk_skin_send_storage_buffer(&creature_instances[s].skin, image_index);

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
    draw.layout = pe_vk_pipeline_layout_skinned;
    draw.command_buffer = *command;
    draw.image_index = image_index;
    pe_vk_draw_model(&draw);
  }
}
