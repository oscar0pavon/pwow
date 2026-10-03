#include "attached.h"

#include "gamedata.h"

#include <engine/animation/animation.h>
#include <engine/array.h>
#include <engine/images.h>
#include <engine/macros.h>
#include <engine/renderer/descriptor_set.h>
#include <engine/renderer/draw.h>
#include <engine/renderer/uniform_buffer.h>
#include <engine/renderer/vk_images.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

//WoW.exe draws a pauldron about half again as big as its model, matched by eye
//against the client's own screenshot
#define SHOULDER_SCALE 1.5f

static PShader attached_shader;

void attached_init(PShader shader) { attached_shader = shader; }

//---------------------------------------------------------------------------
//attachment points
//---------------------------------------------------------------------------

bool attachment_points_load(const char *glb_path, PAttachmentPoints *out) {
  out->count = 0;

  char path[512];
  snprintf(path, sizeof(path), "%s", glb_path);
  char *extension = strrchr(path, '.');
  if (!extension)
    return false;
  strcpy(extension, ".att");

  FILE *file = fopen(path, "r");
  if (!file)
    return false;

  while (out->count < ATTACHMENT_POINTS_MAX) {
    PAttachmentPoint *point = &out->points[out->count];
    if (fscanf(file, "%u %u %f %f %f", &point->id, &point->joint,
               &point->position[0], &point->position[1],
               &point->position[2]) != 5)
      break;
    out->count++;
  }

  fclose(file);
  return true;
}

const PAttachmentPoint *attachment_points_find(const PAttachmentPoints *points,
                                               u32 id) {
  for (int i = 0; i < points->count; i++)
    if (points->points[i].id == id)
      return &points->points[i];
  return NULL;
}

void attachment_matrix(const PSkin *body, const PAttachmentPoint *point,
                       mat4 out) {
  glm_mat4_copy((vec4 *)body->node_uniform.joints_matrix[point->joint], out);
  glm_translate(out, (float *)point->position);
}

//---------------------------------------------------------------------------
//one loaded mesh per model, shared by every instance of it
//---------------------------------------------------------------------------

typedef struct AttachedTemplate {
  char glb_path[256];
  PModel model;
  PSkin skin;
} AttachedTemplate;

#define ATTACHED_TEMPLATES_MAX 32
static AttachedTemplate attached_templates[ATTACHED_TEMPLATES_MAX];
static int attached_template_count;

//an instance copies its template's texture and binds it into its descriptor
//sets as it is made, so the template needs a real one, though no instance
//keeps it
static AttachedTemplate *find_or_load_template(const char *glb_path,
                                               const char *texture_png) {
  for (int i = 0; i < attached_template_count; i++)
    if (strcmp(attached_templates[i].glb_path, glb_path) == 0)
      return &attached_templates[i];

  if (attached_template_count >= ATTACHED_TEMPLATES_MAX)
    return NULL;

  AttachedTemplate *template = &attached_templates[attached_template_count++];
  snprintf(template->glb_path, sizeof(template->glb_path), "%s", glb_path);
  pe_vk_load_skin(&template->skin, &template->model, glb_path);
  template->model.shader = attached_shader;
  pe_load_texture(texture_png, &template->model.texture);
  return template;
}

bool attached_create(PAttachedModel *out, const char *glb_path,
                     const char *texture_png) {
  if (access(glb_path, R_OK) != 0 || access(texture_png, R_OK) != 0)
    return false;

  AttachedTemplate *template = find_or_load_template(glb_path, texture_png);
  if (!template)
    return false;

  pe_vk_skin_instance(&out->skin, &template->skin);
  pe_vk_skin_create_storage_buffers(&out->skin);
  pe_vk_model_instance_skinned(&out->model, &template->model, &out->skin);

  pe_load_texture(texture_png, &out->model.texture);
  pe_vk_descriptor_skinned_update(&out->model, &out->skin, &main_render_target);

  //skinned.vert's weighted sum of joint matrices has no safe zero fallback,
  //and this storage buffer starts zeroed
  if (out->skin.joints.count > 0)
    pe_anim_nodes_update(&out->skin);
  return true;
}

void attached_release(PAttachedModel *attached) {
  vkDeviceWaitIdle(vk_device);

  for (int i = 0; i < attached->model.uniform_buffers_memory.count; i++) {
    VkDeviceMemory *memory = array_get(&attached->model.uniform_buffers_memory, i);
    vkFreeMemory(vk_device, *memory, NULL);
  }
  vkDestroyDescriptorPool(vk_device, attached->model.descriptor_pool, NULL);

  for (int i = 0; i < attached->skin.shader_storage_buffers_memory.count; i++) {
    VkDeviceMemory *memory =
        array_get(&attached->skin.shader_storage_buffers_memory, i);
    vkFreeMemory(vk_device, *memory, NULL);
  }

  pe_vk_clean_image(&attached->model.texture);
}

//---------------------------------------------------------------------------
//items on a body
//---------------------------------------------------------------------------

//"Helm_Leather_A_02.mdx" in folder "head" for a male is the base of its
//converted files, item/objectcomponents/head/helm_leather_a_02_tam, and a
//texture's name is the same without the suffix
static void item_component_base(char *out, size_t size, const char *folder,
                                const char *name, const char *suffix) {
  char lower[64];
  snprintf(lower, sizeof(lower), "%s", name);
  for (char *c = lower; *c; c++)
    *c = (char)tolower((unsigned char)*c);

  size_t length = strlen(lower);
  if (length > 4 && strcmp(lower + length - 4, ".mdx") == 0)
    lower[length - 4] = '\0';

  snprintf(out, size, "item/objectcomponents/%s/%s%s", folder, lower, suffix);
}

bool attached_item_create(PAttachedItem *out, const PAttachmentPoints *points,
                          u32 point_id, const char *folder, const char *suffix,
                          const char *model, const char *texture) {
  if (model[0] == '\0' || texture[0] == '\0')
    return false;

  const PAttachmentPoint *point = attachment_points_find(points, point_id);
  if (!point)
    return false;

  char model_base[256], texture_base[256];
  item_component_base(model_base, sizeof(model_base), folder, model, suffix);
  item_component_base(texture_base, sizeof(texture_base), folder, texture, "");
  if (!gamedata_ensure_model(model_base) || !gamedata_ensure_png(texture_base))
    return false;

  char glb_path[300], texture_path[300];
  snprintf(glb_path, sizeof(glb_path), "data/%s.glb", model_base);
  snprintf(texture_path, sizeof(texture_path), "data/%s.png", texture_base);
  if (!attached_create(&out->model, glb_path, texture_path))
    return false;

  out->point = *point;
  glm_mat4_identity(out->local);
  if (point_id == ATTACHMENT_SHOULDER_LEFT ||
      point_id == ATTACHMENT_SHOULDER_RIGHT)
    glm_scale_uni(out->local, SHOULDER_SCALE);
  return true;
}

//from WoWee's weaponLocalTransform(), tuned there against the real client. a
//weapon model is long along its own X, which is the character's front to back.
//those turns are in the model's own axes and the glb's are a quarter about X
//from them (m22gltf's remap), so they are carried over by that turn
void attached_carry_matrix(bool big_weapon, mat4 out) {
  glm_mat4_identity(out);

  if (big_weapon) {
    float tx = -0.03f, ty = -0.10f, tz = 0.15f, cant = 33.0f, scale = 1.3f;

    glm_translate(out, (vec3){tx, ty, tz});
    glm_rotate(out, glm_rad(cant), (vec3){1, 0, 0});
    glm_rotate(out, glm_rad(90.0f), (vec3){0, 1, 0});
    glm_rotate(out, glm_rad(90.0f), (vec3){1, 0, 0});
    glm_scale_uni(out, scale);
  } else {
    glm_rotate(out, glm_rad(90.0f), (vec3){0, 1, 0});
  }

  mat4 remap, remap_inverse, carried;
  glm_mat4_identity(remap);
  glm_rotate(remap, glm_rad(-90.0f), (vec3){1, 0, 0});
  glm_mat4_identity(remap_inverse);
  glm_rotate(remap_inverse, glm_rad(90.0f), (vec3){1, 0, 0});
  glm_mat4_mul(remap, out, carried);
  glm_mat4_mul(carried, remap_inverse, out);
}

void attached_item_draw(PAttachedItem *item, const PSkin *body,
                        mat4 body_model_mat, VkCommandBuffer *command,
                        uint32_t image_index, mat4 view, mat4 projection) {
  mat4 attachment;
  attachment_matrix(body, &item->point, attachment);
  mat4 at_point;
  glm_mat4_mul(body_model_mat, attachment, at_point);
  glm_mat4_mul(at_point, item->local, item->model.model.model_mat);
  skinned_model_draw(&item->model.model, &item->model.skin, command,
                     image_index, view, projection);
}

//---------------------------------------------------------------------------
//drawing
//---------------------------------------------------------------------------

void skinned_model_draw(PModel *model, PSkin *skin, VkCommandBuffer *command,
                        uint32_t image_index, mat4 view, mat4 projection) {
  //this model's own joint matrices, computed for this frame by
  //play_animation_list() off its own animation clock
  pe_vk_skin_send_storage_buffer(skin, image_index);

  PUniformBufferObject *ubo = &model->uniform_buffer_object;
  glm_mat4_copy(model->model_mat, ubo->model);
  glm_mat4_copy(view, ubo->view);
  glm_mat4_copy(projection, ubo->projection);

  //same guessed-sun-direction light placement player_draw() uses
  vec3 light_position;
  glm_vec3_copy(model->model_mat[3], light_position);
  glm_vec3_muladds((vec3){0.4f, -0.3f, 0.8f}, 5000.0f, light_position);
  glm_vec4(light_position, 1, ubo->light_position);

  pe_vk_send_uniform_buffer(model, image_index);

  PDrawModelCommand draw;
  ZERO(draw);
  draw.model = model;
  draw.layout = pe_vk_pipeline_layout_skinned;
  draw.command_buffer = *command;
  draw.image_index = image_index;
  pe_vk_draw_model(&draw);
}
