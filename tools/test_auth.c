//exercises pengine's wowauth/wowworld modules against a real realmd and
//mangosd: logs in over SRP6, prints the realm list, then connects to the
//first realm's world server and completes the CMSG_AUTH_SESSION handshake.
//usage: test_auth <host> <port> <account> <password>
#include <engine/wowauth/wowauth.h>
#include <engine/wowauth/wowworld.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "usage: %s <host> <port> <account> <password>\n", argv[0]);
    return 1;
  }

  const char *host = argv[1];
  int port = atoi(argv[2]);
  const char *account = argv[3];
  const char *password = argv[4];

  PWowAuthResult result;
  bool ok = pe_wowauth_login(host, port, account, password, &result);

  if (!ok) {
    fprintf(stderr, "login failed: %s\n", result.error);
    return 1;
  }

  printf("login succeeded\n");
  printf("session key: ");
  for (int i = 0; i < 40; i++)
    printf("%02x", result.session_key[i]);
  printf("\n");

  printf("%d realm(s):\n", result.realm_count);
  for (int i = 0; i < result.realm_count; i++) {
    PWowRealm *realm = &result.realms[i];
    printf("  [%d] %s @ %s (icon=%d flags=0x%02x population=%.2f)\n", realm->id,
           realm->name, realm->address, realm->icon, realm->flags,
           realm->population);
  }

  if (result.realm_count == 0)
    return 0;

  PWowRealm *realm = &result.realms[0];
  char realm_host[64];
  int realm_port;
  char *colon = strchr(realm->address, ':');
  if (!colon) {
    fprintf(stderr, "realm address '%s' has no port\n", realm->address);
    return 1;
  }
  snprintf(realm_host, sizeof(realm_host), "%.*s",
          (int)(colon - realm->address), realm->address);
  realm_port = atoi(colon + 1);

  printf("\nconnecting to world server %s:%d ...\n", realm_host, realm_port);

  PWowWorld world;
  char world_error[PE_WOWWORLD_ERROR_MAX];
  bool world_ok = pe_wowworld_connect(realm_host, realm_port, account,
                                      result.session_key, 5875, realm->id,
                                      &world, world_error, sizeof(world_error));
  if (!world_ok) {
    fprintf(stderr, "world connect failed: %s\n", world_error);
    return 1;
  }

  printf("world session established (AUTH_OK)\n");

  PWowCharacter characters[PE_WOWWORLD_CHARACTERS_MAX];
  int char_count = 0;
  if (!pe_wowworld_char_enum(&world, characters, PE_WOWWORLD_CHARACTERS_MAX,
                             &char_count, world_error, sizeof(world_error))) {
    fprintf(stderr, "char enum failed: %s\n", world_error);
    pe_wowworld_close(&world);
    return 1;
  }

  printf("%d character(s):\n", char_count);
  for (int i = 0; i < char_count; i++)
    printf("  [%llu] %s\n", (unsigned long long)characters[i].guid,
           characters[i].name);

  if (char_count == 0) {
    pe_wowworld_close(&world);
    return 0;
  }

  printf("\nlogging in as %s ...\n", characters[0].name);

  PWowLoginResult login;
  if (!pe_wowworld_player_login(&world, characters[0].guid, &login,
                                world_error, sizeof(world_error))) {
    fprintf(stderr, "player login failed: %s\n", world_error);
    pe_wowworld_close(&world);
    return 1;
  }

  printf("in the world: map=%u position=(%.2f, %.2f, %.2f) facing=%.2f\n",
         login.map, login.x, login.y, login.z, login.o);

  pe_wowworld_close(&world);

  return 0;
}
