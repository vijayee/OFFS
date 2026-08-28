//
// Created by victor on 5/28/26.
//

#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include "Util/base58.h"
#include <cbor.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Result codes for read_file_contents. */
#define READ_FILE_OK          0
#define READ_FILE_IO_ERROR   -1  /* errno meaningful; already perror'd */
#define READ_FILE_EMPTY      -2  /* zero-length file */
#define READ_FILE_TOO_LARGE  -3  /* exceeds the QR payload cap */

/* A QR image (PPM) is at most ~1.7 MB; the peer-info wire payload caps at
 * 2 MB. Reject anything larger before allocating. */
#define QR_FILE_MAX_BYTES (2u * 1024u * 1024u)

/* Read an entire file into a malloc'd buffer. Returns READ_FILE_OK on
 * success (caller frees *out_data). I/O failures are reported with perror
 * here and return READ_FILE_IO_ERROR; empty and oversized files return
 * READ_FILE_EMPTY / READ_FILE_TOO_LARGE so the caller can print a specific
 * message. */
static int read_file_contents(const char* path, uint8_t** out_data,
                              size_t* out_size) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    perror(path);
    return READ_FILE_IO_ERROR;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    perror("fseek");
    fclose(file);
    return READ_FILE_IO_ERROR;
  }
  long file_length = ftell(file);
  if (file_length < 0) {
    perror("ftell");
    fclose(file);
    return READ_FILE_IO_ERROR;
  }
  if (file_length == 0) {
    fclose(file);
    return READ_FILE_EMPTY;
  }
  if ((size_t)file_length > QR_FILE_MAX_BYTES) {
    fclose(file);
    return READ_FILE_TOO_LARGE;
  }
  rewind(file);
  uint8_t* buffer = (uint8_t*)malloc((size_t)file_length);
  if (buffer == NULL) {
    perror("malloc");
    fclose(file);
    return READ_FILE_IO_ERROR;
  }
  if (fread(buffer, 1, (size_t)file_length, file) != (size_t)file_length) {
    perror("fread");
    free(buffer);
    fclose(file);
    return READ_FILE_IO_ERROR;
  }
  fclose(file);
  *out_data = buffer;
  *out_size = (size_t)file_length;
  return READ_FILE_OK;
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
        fprintf(stderr, "%s\n", L10N_PEER_INFO_USAGE);
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
    if (type != CLIENT_API_PEER_INFO_RESPONSE) {
      fprintf(stderr, "%s: unexpected response type %u\n", L10N_ERROR, type);
      cbor_decref(&response);
      return 1;
    }

    client_api_peer_info_response_t peer_resp;
    memset(&peer_resp, 0, sizeof(peer_resp));
    if (client_api_peer_info_response_decode(response, &peer_resp) != 0) {
      fprintf(stderr, "%s\n", L10N_PEER_INFO_DECODE);
      cbor_decref(&response);
      return 1;
    }
    cbor_decref(&response);

    bool command_failed = false;
    if (format == 2) {
      /* Write the PPM image to qr_path ("-" = stdout) */
      FILE* out = (strcmp(qr_path, "-") == 0) ? stdout
                                              : fopen(qr_path, "wb");
      if (out == NULL) {
        perror(qr_path);
        command_failed = true;
      } else {
        size_t written = fwrite(peer_resp.data, 1, peer_resp.data_size, out);
        if (written < peer_resp.data_size) {
          fprintf(stderr, "%s: short write (%zu/%zu bytes)\n",
                  L10N_ERROR, written, peer_resp.data_size);
          command_failed = true;
        }
        if (out != stdout) fclose(out);
      }
    } else {
      size_t b58_len = base58_encoded_length(peer_resp.data_size) + 1;
      char* b58 = (char*)malloc(b58_len);
      if (b58 == NULL) {
        perror("malloc");
        command_failed = true;
      } else {
        int enc_rc = base58_encode(peer_resp.data, peer_resp.data_size,
                          b58, b58_len);
        if (enc_rc > 0) {
          b58[enc_rc] = '\0';
          printf("%s\n", L10N_PEER_INFO_PROMPT);
          printf("  Data: %s\n", b58);
        } else {
          fprintf(stderr, "%s\n", L10N_ERROR);
          command_failed = true;
        }
        free(b58);
      }
    }
    client_api_peer_info_response_destroy(&peer_resp);
    return command_failed ? 1 : 0;
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
      int read_rc = read_file_contents(argv[2], &file_data, &file_size);
      if (read_rc == READ_FILE_EMPTY) {
        fprintf(stderr, L10N_PUT_EMPTY_FILE "\n");
        return 1;
      }
      if (read_rc == READ_FILE_TOO_LARGE) {
        fprintf(stderr, "%s: %s\n", argv[2], L10N_FILE_TOO_LARGE);
        return 1;
      }
      if (read_rc != READ_FILE_OK) {
        return 1;  /* details already reported via perror */
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
