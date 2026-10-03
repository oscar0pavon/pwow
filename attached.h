#ifndef PWOW_ATTACHED_H
#define PWOW_ATTACHED_H

#include <cglm/cglm.h>
#include <engine/model.h>
#include <engine/numbers.h>
#include <engine/renderer/vulkan.h>
#include <engine/skeletal.h>

#include <stdbool.h>

//a model drawn apart from a body, at one of its attachment points: a helm, a
//shoulder, a weapon. it is a skinned model of its own, placed each frame by
//whoever owns the body, from the body's joint matrices and the point

//where a body's gear rides: one of the points m22gltf writes to <model>.att,
//the joint it follows and a position in the glb's own axes. the ids are the
//game's: 0 the arm a shield is worn on, 1 and 2 the hands, 5 and 6 the
//shoulders, 11 the helm
#define ATTACHMENT_SHIELD 0
#define ATTACHMENT_RIGHT_HAND 1
#define ATTACHMENT_LEFT_HAND 2
#define ATTACHMENT_HELM 11

typedef struct PAttachmentPoint {
  u32 id, joint;
  vec3 position;
} PAttachmentPoint;

#define ATTACHMENT_POINTS_MAX 64

typedef struct PAttachmentPoints {
  PAttachmentPoint points[ATTACHMENT_POINTS_MAX];
  int count;
} PAttachmentPoints;

//reads the .att beside glb_path. false if there is none, which leaves out
//empty: a body converted before m22gltf wrote them has to be converted again
bool attachment_points_load(const char *glb_path, PAttachmentPoints *out);

//NULL if the body has no point of that id
const PAttachmentPoint *attachment_points_find(const PAttachmentPoints *points,
                                               u32 id);

//the matrix that carries a model made at the origin of a point onto the body,
//in the body's own axes: the point's joint as it is posed now, then the offset
//to the point. the body's own placement still goes in front
void attachment_matrix(const PSkin *body, const PAttachmentPoint *point,
                       mat4 out);

//the shader every attached model draws with, made by whoever owns the
//pipeline for the bodies. call once, before the first attached_create()
void attached_init(PShader shader);

typedef struct PAttachedModel {
  PModel model;
  PSkin skin;
} PAttachedModel;

//an instance of the model in glb_path, drawn with the texture in texture_png,
//both already in data/: the first of a model loads it, and the rest share its
//mesh and own a skeleton and a texture. false if either file is not there
bool attached_create(PAttachedModel *out, const char *glb_path,
                     const char *texture_png);

//gives back what the instance owns, never the mesh it shares
void attached_release(PAttachedModel *attached);

//draws a skinned model at its model_mat, lit from the usual guessed sun
void skinned_model_draw(PModel *model, PSkin *skin, VkCommandBuffer *command,
                        uint32_t image_index, mat4 view, mat4 projection);

#endif
