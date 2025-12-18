#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include "sham.h"

#define BUFFER_SIZE 4096
#define DEFAULT_PORT 8080

// Global variables for packet loss simulation

float g_loss_rate = 0.0f;

// Calculate MD5 checksum of a file using modern OpenSSL EVP interface
void calculate_file_md5(const char *filename)
{
    FILE *file = fopen(filename, "rb");
    if (!file)
    {
        printf("MD5: ERROR - Cannot open file for checksum\n");
        return;
    }

    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx)
    {
        printf("MD5: ERROR - Cannot create digest context\n");
        fclose(file);
        return;
    }

    if (EVP_DigestInit_ex(md_ctx, EVP_md5(), NULL) != 1)
    {
        printf("MD5: ERROR - Cannot initialize digest\n");
        EVP_MD_CTX_free(md_ctx);
        fclose(file);
        return;
    }

    unsigned char buffer[1024];
    size_t bytes_read;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        if (EVP_DigestUpdate(md_ctx, buffer, bytes_read) != 1)
        {
            printf("MD5: ERROR - Cannot update digest\n");
            EVP_MD_CTX_free(md_ctx);
            fclose(file);
            return;
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len;

    if (EVP_DigestFinal_ex(md_ctx, digest, &digest_len) != 1)
    {
        printf("MD5: ERROR - Cannot finalize digest\n");
        EVP_MD_CTX_free(md_ctx);
        fclose(file);
        return;
    }

    EVP_MD_CTX_free(md_ctx);
    fclose(file);

    // Print MD5 in the required format
    unsigned int i;
    printf("MD5: ");
    for (i = 0; i < digest_len; i++)
    {
        printf("%02x", digest[i]);
    }
    printf("\n");
    fflush(stdout);
}

int handle_file_transfer(struct sham_connection *conn)
{

    // First, receive the filename length (1 byte)
    uint8_t filename_len;
    if (sham_recv(conn, &filename_len, 1) != 1)
    {
        return -1;
    }

    // Then receive the filename of exact length
    char filename[256];
    // filename_len is uint8_t (0-255), sizeof(filename) is 256, so check is unnecessary

    int name_received = sham_recv(conn, filename, filename_len);
    if (name_received != filename_len)
    {
        return -1;
    }
    filename[filename_len] = '\0';

    // Receive the file
    int result = sham_recv_file(conn, filename);
    if (result < 0)
    {
        return -1;
    }

    // Calculate and print MD5 checksum
    calculate_file_md5(filename);

    return 0;
}

int handle_chat_mode(struct sham_connection *conn)
{
    printf("[CHAT] Client connected, starting interactive chat session\n");

    char buffer[BUFFER_SIZE];
    char input_buffer[BUFFER_SIZE];

    while (1)
    {
        fd_set read_fds;
        int max_fd;

        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds); // stdin
        FD_SET(conn->sockfd, &read_fds); // socket

        max_fd = (STDIN_FILENO > conn->sockfd) ? STDIN_FILENO : conn->sockfd;

        // Use select to monitor both stdin and socket
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);

        if (activity < 0)
        {
            if (errno == EINTR)
                continue; // Interrupted by signal
            perror("select error");
            break;
        }

        // Check if there's input from stdin
        if (FD_ISSET(STDIN_FILENO, &read_fds))
        {
            if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL)
            {
                // Remove newline
                input_buffer[strcspn(input_buffer, "\n")] = 0;

                if (strcmp(input_buffer, "/quit") == 0)
                {
                    printf("[CHAT] Server initiating chat termination...\n");
                    return 0; // This will trigger connection close
                }

                if (strlen(input_buffer) > 0)
                {
                    int sent = sham_send(conn, input_buffer, strlen(input_buffer));
                    if (sent < 0)
                    {
                        printf("[CHAT] Failed to send message to client\n");
                        break;
                    }
                }
            }
        }

        // Check if there's data from the socket
        if (FD_ISSET(conn->sockfd, &read_fds))
        {
            int received = sham_recv(conn, buffer, sizeof(buffer) - 1);
            if (received <= 0)
            {
                printf("[CHAT] Client disconnected\n");
                break;
            }

            buffer[received] = '\0';

            // Check if client sent /quit
            if (strcmp(buffer, "/quit") == 0)
            {
                printf("[CHAT] Client requested to quit\n");
                break;
            }

            printf("[Client]: %s\n", buffer);
        }
    }

    printf("[CHAT] Chat session ended\n");
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        // print_usage(argv[0]);
        return 1;
    }

    int port;
    bool chat_mode = false;

    // Parse command line arguments
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
        // print_usage(argv[0]);
        return 0;
    }

    // First argument must be port
    port = atoi(argv[1]);
    if (port <= 0)
    {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        // print_usage(argv[0]);
        return 1;
    }

    // Parse optional arguments
    {
        int i;
        for (i = 2; i < argc; i++)
        {
            if (strcmp(argv[i], "--chat") == 0)
            {
                chat_mode = true;
            }
            else
            {
                // Try to parse as loss rate
                float loss_rate = atof(argv[i]);
                if (loss_rate >= 0.0f && loss_rate <= 1.0f)
                {
                    g_loss_rate = loss_rate;
                }
                else
                {
                    fprintf(stderr, "Invalid loss rate: %s (must be between 0.0 and 1.0)\n", argv[i]);
                    return 1;
                }
            }
        }
    }

    // Create listening connection
    struct sham_connection *listen_conn = sham_create_connection();
    if (!listen_conn)
    {
        fprintf(stderr, "Failed to create listening connection\n");
        return 1;
    }

    // Set loss rate for the connection
    listen_conn->loss_rate = g_loss_rate;

    // Initialize verbose logging if enabled
    listen_conn->verbose_log_file = sham_open_verbose_log("server");

    // Start listening
    if (sham_listen(listen_conn, port) < 0)
    {
        fprintf(stderr, "Failed to start listening on port %d\n", port);
        sham_free_connection(listen_conn);
        return 1;
    }

    // Accept and handle clients
    int client_count = 0;

    while (1)
    {
        // Check if listening socket is still valid
        if (listen_conn->sockfd < 0)
        {
            fprintf(stderr, "ERROR: Listening socket failed, server shutting down\n");
            break;
        }

        struct sham_connection *client_conn = sham_accept(listen_conn);
        if (!client_conn)
        {
            // Check if it's a socket failure or just no connection
            if (listen_conn->sockfd < 0)
            {
                fprintf(stderr, "ERROR: Listening socket failed during accept, server shutting down\n");
                break;
            }
            continue;
        }

        // Set loss rate for client connection
        client_conn->loss_rate = g_loss_rate;

        // Copy verbose log file from listening connection
        client_conn->verbose_log_file = listen_conn->verbose_log_file;

        client_count++;

        // Handle client based on mode
        if (chat_mode)
        {
            handle_chat_mode(client_conn);
        }
        else
        {
            handle_file_transfer(client_conn);
        }

        // Close client connection
        sham_close(client_conn);

        client_conn->verbose_log_file = NULL;
        client_conn->sockfd = -1;
        sham_free_connection(client_conn);
    }
    sham_free_connection(listen_conn);

    return 0;
}
