//exercises pengine's wowauth module against a real realmd: logs in over SRP6
//and prints the realm list it gets back. usage: test_auth <host> <port>
//<account> <password>
#include <engine/wowauth/wowauth.h>

#include <stdio.h>
#include <stdlib.h>

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

  return 0;
}
