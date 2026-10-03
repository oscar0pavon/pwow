#include "gamedata.h"

#include <engine/macros.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define GAME_DATA_PATH_DEFAULT "/root/sources/WoWee/Data/expansions/classic"
#define BLP_CONVERT_PATH_DEFAULT "/root/sources/WoWee/build/bin/blp_convert"
#define M22GLTF_PATH_DEFAULT "./m22gltf"
#define ANIMATION_DATA_PATH_DEFAULT \
  "/root/sources/vmangos/run/bin/5875/dbc/AnimationData.dbc"

static const char *setting(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value ? value : fallback;
}

//creates path and every missing parent directory, tolerating "already exists"
static void make_directories(const char *path) {
  char buf[512];
  snprintf(buf, sizeof(buf), "%s", path);
  for (char *p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(buf, 0755);
      *p = '/';
    }
  }
  mkdir(buf, 0755);
}

static bool copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in)
    return false;
  FILE *out = fopen(to, "wb");
  if (!out) {
    fclose(in);
    return false;
  }
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, n, out);
  fclose(in);
  fclose(out);
  return true;
}

//runs argv[0] with argv, no shell involved - argv[] is built from a fixed
//tool path plus a name that ultimately comes off the wire (ItemDisplayInfo.dbc,
//read by entry the local server names), so this never goes through a shell to
//interpolate it into. blocks for the child; true if it exited 0
static bool run_tool(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
    }
    execv(argv[0], argv);
    _exit(127);
  }
  int status;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool gamedata_ensure_png(const char *base) {
  char png[512];
  snprintf(png, sizeof(png), "data/%s.png", base);
  if (access(png, F_OK) == 0)
    return true;

  char source_blp[512];
  snprintf(source_blp, sizeof(source_blp), "%s/%s.blp",
           setting("GAME_DATA", GAME_DATA_PATH_DEFAULT), base);
  if (access(source_blp, F_OK) != 0)
    return false;

  char dest_blp[512];
  snprintf(dest_blp, sizeof(dest_blp), "data/%s.blp", base);
  make_directories(dest_blp);
  if (!copy_file(source_blp, dest_blp))
    return false;

  char *argv[] = {(char *)setting("BLP_CONVERT", BLP_CONVERT_PATH_DEFAULT),
                  "--to-png", dest_blp, NULL};
  bool ok = run_tool(argv);
  remove(dest_blp);
  return ok && access(png, F_OK) == 0;
}

bool gamedata_ensure_model(const char *base) {
  char glb[512];
  snprintf(glb, sizeof(glb), "data/%s.glb", base);
  if (access(glb, F_OK) == 0)
    return true;

  char source_m2[512];
  snprintf(source_m2, sizeof(source_m2), "%s/%s.m2",
           setting("GAME_DATA", GAME_DATA_PATH_DEFAULT), base);
  if (access(source_m2, F_OK) != 0)
    return false;

  char model[512];
  snprintf(model, sizeof(model), "%s.m2", base);
  char *argv[] = {(char *)setting("M22GLTF", M22GLTF_PATH_DEFAULT),
                  (char *)setting("GAME_DATA", GAME_DATA_PATH_DEFAULT),
                  model,
                  "data",
                  (char *)setting("ANIMATION_DATA", ANIMATION_DATA_PATH_DEFAULT),
                  NULL};
  return run_tool(argv) && access(glb, F_OK) == 0;
}
