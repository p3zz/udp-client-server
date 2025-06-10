#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>

#define PORT 12345
#define BUFFER_SIZE 1024
#define RESPONSE "Message received"

typedef struct{
    int value;
    pthread_mutex_t lock;
}shared_data;

static shared_data data = { .value = 0, .lock = PTHREAD_MUTEX_INITIALIZER };

void *thread_body(void *arg) {
    shared_data *shared = (shared_data *)arg;

    while (1) {
        pthread_mutex_lock(&shared->lock);
        shared->value++;
        shared->value %= 10;
        pthread_mutex_unlock(&shared->lock);

        sleep(2); // simulate work / wait time
    }

    return NULL;
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUFFER_SIZE];
    char client_ip[INET_ADDRSTRLEN];
    socklen_t addr_len = sizeof(client_addr);
    pthread_t tid;

    if (pthread_create(&tid, NULL, thread_body, &data) != 0) {
        perror("Failed to create thread");
        return 1;
    }

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Accept from any interface
    server_addr.sin_port = htons(PORT);

    // Bind socket to the address
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Listening for UDP messages on port %d...\n", PORT);

    while (1) {
        ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                             (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }

        buffer[n] = '\0';  // Null-terminate
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);

        printf("Received message from %s:%d: %s\n",
               client_ip, ntohs(client_addr.sin_port), buffer);

        int value = 0;
        pthread_mutex_lock(&data.lock);
        value = data.value;
        pthread_mutex_unlock(&data.lock);

        // Send response
        if (sendto(sockfd, &value, sizeof(value), 0,
                   (struct sockaddr *)&client_addr, addr_len) < 0) {
            perror("sendto (response) failed");
        } else {
            printf("Sent response to %s:%d\n",
                   client_ip, ntohs(client_addr.sin_port));
        }
    }

    close(sockfd);
    return 0;
}
