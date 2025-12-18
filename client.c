#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include "sham.h"

#define BUFFER_SIZE 4096

// Global variables for packet loss simulation

float g_loss_rate = 0.0f;

int run_file_transfer_mode(struct sham_connection *conn, const char *input_file, const char *output_file)
{
    printf("\n=== S.H.A.M. File Transfer Mode ===\n");
    printf("Sending file '%s' to be saved as '%s' on server\n", input_file, output_file);

    // First, send the filename length (1 byte)
    uint8_t filename_len = (uint8_t)strlen(output_file);
    if (filename_len != strlen(output_file))
    {
        fprintf(stderr, "Filename too long (max 255 bytes)\n");
        return -1;
    }

    if (sham_send(conn, &filename_len, 1) != 1)
    {
        fprintf(stderr, "Failed to send filename length to server\n");
        return -1;
    }

    // Then send the output filename to server
    int sent = sham_send(conn, output_file, strlen(output_file));
    if (sent < 0)
    {
        fprintf(stderr, "Failed to send filename to server\n");
        return -1;
    }

    // Send the file content
    int result = sham_send_file(conn, input_file);
    if (result < 0)
    {
        fprintf(stderr, "Failed to send file\n");
        return -1;
    }

    return 0;
}

int run_chat_mode(struct sham_connection *conn)
{
    printf("\n=== S.H.A.M. Chat Mode ===\n");
    printf("Type messages to send. Type '/quit' to exit.\n\n");

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
                    printf("[CHAT] Initiating chat termination...\n");
                    // Send /quit to server to trigger FIN handshake
                    sham_send(conn, "/quit", strlen("/quit"));
                    return 0;
                }

                if (strlen(input_buffer) > 0)
                {
                    printf("[YOU]: %s\n", input_buffer);

                    // Send message to server
                    int sent = sham_send(conn, input_buffer, strlen(input_buffer));
                    if (sent < 0)
                    {
                        fprintf(stderr, "Failed to send message to server\n");
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
                printf("[CHAT] Server disconnected\n");
                break;
            }

            buffer[received] = '\0';
            printf("[Server]: %s\n", buffer);
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        // print_usage(argv[0]);
        return 1;
    }

    const char *server_ip;
    int server_port;
    bool chat_mode = false;
    const char *input_file = NULL;
    const char *output_file = NULL;

    server_ip = argv[1];
    server_port = atoi(argv[2]);

    // Parse command line arguments
    if (argc >= 4)
    {
        if (strcmp(argv[3], "--chat") == 0)
        {
            chat_mode = true;

            // Check for loss rate after --chat
            if (argc >= 5)
            {
                float loss_rate = atof(argv[4]);
                if (loss_rate >= 0.0f && loss_rate <= 1.0f)
                {
                    g_loss_rate = loss_rate;
                }
                else
                {
                    fprintf(stderr, "Invalid loss rate: %s (must be between 0.0 and 1.0)\n", argv[4]);
                    return 1;
                }
            }
        }
        else
        {
            // File transfer mode - need input_file and output_file
            if (argc < 5)
            {
                fprintf(stderr, "File transfer mode requires input_file and output_file_name\n");
                // print_usage(argv[0]);
                return 1;
            }

            input_file = argv[3];
            output_file = argv[4];

            // Check for optional loss rate
            if (argc >= 6)
            {
                float loss_rate = atof(argv[5]);
                if (loss_rate >= 0.0f && loss_rate <= 1.0f)
                {
                    g_loss_rate = loss_rate;
                }
                else
                {
                    fprintf(stderr, "Invalid loss rate: %s (must be between 0.0 and 1.0)\n", argv[5]);
                    return 1;
                }
            }
        }
    }
    else
    {
        fprintf(stderr, "Missing arguments\n");
        // print_usage(argv[0]);
        return 1;
    }

    // Create connection
    struct sham_connection *conn = sham_create_connection();
    if (!conn)
    {
        fprintf(stderr, "Failed to create connection\n");
        return 1;
    }

    // Set loss rate for the connection
    conn->loss_rate = g_loss_rate;

    // Initialize verbose logging
    conn->verbose_log_file = sham_open_verbose_log("client");

    // Connect to server
    if (sham_connect(conn, server_ip, server_port) < 0)
    {
        fprintf(stderr, "Failed to connect to server\n");
        sham_free_connection(conn);
        return 1;
    }

    // Run based on mode
    int result = 0;

    if (chat_mode)
    {
        result = run_chat_mode(conn);
    }
    else
    {
        result = run_file_transfer_mode(conn, input_file, output_file);
    }

    // Close connection
    sham_close(conn);

    // Cleanup
    sham_free_connection(conn);

    return result;
}
