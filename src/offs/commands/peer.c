//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include "Util/base58.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read an entire file into a malloc'd buffer. Returns 0 on success (caller
 * frees *out_data), -1 on failure. */
static int read_file_contents(const char* path, uint8_t** out_data,
                              size_t* out_size) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) return -1;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return -1;
  }
  long file_length = ftell(file);
  if (file_length < 0) {
    fclose(file);
    return -1;
  }
  rewind(file);
  uint8_t* buffer = (uint8_t*)malloc((size_t)file_length);
  if (buffer == NULL) {
    fclose(file);
    return -1;
  }
  if (fread(buffer, 1, (size_t)file_length, file) != (size_t)file_length) {
    free(buffer);
    fclose(file);
    return -1;
  }
  fclose(file);
  *out_data = buffer;
  *out_size = (size_t)file_length;
  return 0;
}

int cmd_peer(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    printf("Usage: offs peer <info|list|connect> ...\n");
    return 1;
  }

  const char* subcommand = argv[0];

  if (strcmp(subcommand, "info") == 0) {
    uint8_t format = 0;
    const char* qr_path = NULL;
    for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--qr") == 0 && i + 1 < argc) {
        format = 2;   /* PPM QR image */
        qr_path = argv[++i];
      } else {
        printf("Usage: offs peer info [--qr <file>|-]\n");
        return 1;
      }
    }

    cbor_item_t* request = client_api_peer_info_request_encode_format(format);
    if (request == NULL) {
      fprintf(stderr, "%s\n", L10N_ERROR);
      return 1;
    }
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_PEER_INFO_RESPONSE) {
        client_api_peer_info_response_t peer_resp;
        memset(&peer_resp, 0, sizeof(peer_resp));
        if (client_api_peer_info_response_decode(response, &peer_resp) == 0) {
          if (format == 2) {
            /* Write the PPM image to qr_path ("-" = stdout) */
            FILE* out = (strcmp(qr_path, "-") == 0) ? stdout
                                                    : fopen(qr_path, "wb");
            if (out == NULL) {
              fprintf(stderr, "cannot open %s\n", qr_path);
            } else {
              fwrite(peer_resp.data, 1, peer_resp.data_size, out);
              if (out != stdout) fclose(out);
            }
          } else {
            size_t b58_len = base58_encoded_length(peer_resp.data_size) + 1;
            char* b58 = (char*)malloc(b58_len);
            if (b58 != NULL) {
              int enc_rc = base58_encode(peer_resp.data, peer_resp.data_size,
                                b58, b58_len);
              if (enc_rc > 0) {
                b58[enc_rc] = '\0';
                printf("%s\n", L10N_PEER_INFO_PROMPT);
                printf("  Data: %s\n", b58);
              }
              free(b58);
            }
          }
          client_api_peer_info_response_destroy(&peer_resp);
        }
      } else if (type == CLIENT_API_ERROR) {
        client_api_error_t err_msg;
        memset(&err_msg, 0, sizeof(err_msg));
        if (client_api_error_decode(response, &err_msg) == 0) {
          fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
          client_api_error_destroy(&err_msg);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "list") == 0) {
    cbor_item_t* request = client_api_peer_list_request_encode();
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);

    if (response != NULL) {
      uint8_t type = client_api_wire_get_type(response);
      if (type == CLIENT_API_PEER_LIST_RESPONSE) {
        client_api_peer_list_response_t peer_list;
        memset(&peer_list, 0, sizeof(peer_list));
        if (client_api_peer_list_response_decode(response, &peer_list) == 0) {
          printf("%s\n", L10N_PEER_LIST_PROMPT);
          if (peer_list.peers != NULL) {
            printf("  %zu peer(s)\n", cbor_array_size(peer_list.peers));
          }
          client_api_peer_list_response_destroy(&peer_list);
        }
      }
      cbor_decref(&response);
    }
    return 0;
  }

  if (strcmp(subcommand, "connect") == 0) {
    if (argc < 2) {
      fprintf(stderr, "%s\n", L10N_PEER_CONNECT_USAGE);
      return 1;
    }

    uint8_t format = 1;  /* default: base58 text (existing behavior) */
    uint8_t* file_data = NULL;
    size_t file_size = 0;

    if (strcmp(argv[1], "--qr") == 0) {
      if (argc < 3) {
        fprintf(stderr, "%s\n", L10N_PEER_CONNECT_USAGE);
        return 1;
      }
      if (read_file_contents(argv[2], &file_data, &file_size) != 0) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
      }
      format = 2;
    }

    client_api_peer_connect_t peer_con;
    memset(&peer_con, 0, sizeof(peer_con));
    peer_con.format = format;
    peer_con.data = file_data != NULL ? file_data : (uint8_t*)argv[1];
    peer_con.data_size = file_data != NULL ? file_size : strlen(argv[1]);

    cbor_item_t* request = client_api_peer_connect_encode(&peer_con);
    /* encode copies the payload (cbor_build_bytestring), so the file buffer
     * is no longer needed once the request frame exists. */
    free(file_data);
    file_data = NULL;

    if (request == NULL) {
      fprintf(stderr, "%s\n", L10N_ERROR);
      return 1;
    }
    cbor_item_t* response = cli_client_send(client, request);
    cbor_decref(&request);

    if (response == NULL) {
      fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
      return 1;
    }
    uint8_t type = client_api_wire_get_type(response);
    if (type == CLIENT_API_ERROR) {
      client_api_error_t err_msg;
      memset(&err_msg, 0, sizeof(err_msg));
      if (client_api_error_decode(response, &err_msg) == 0) {
        fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
        client_api_error_destroy(&err_msg);
      }
      cbor_decref(&response);
      return 1;
    }
    cbor_decref(&response);
    printf("%s\n", L10N_OK);
    return 0;
  }

  printf("Usage: offs peer <info|list|connect> ...\n");
  return 1;
}
