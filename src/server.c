#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
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

int has_ip_address(int socket_fd, const char *iface_name) {
    struct ifreq ifr = {0};

    strncpy(ifr.ifr_name, iface_name, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(socket_fd, SIOCGIFADDR, &ifr) == -1) {
        // Interface probably doesn't have an IP address
        return 1;
    }
    struct sockaddr_in *ipaddr = (struct sockaddr_in *)&ifr.ifr_addr;
    printf("Interface %s has IP: %s\n", iface_name, inet_ntoa(ipaddr->sin_addr));
    return 0; // Has IP
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUFFER_SIZE];
    char control[CMSG_SPACE(sizeof(struct in_pktinfo))];
    char client_ip[INET_ADDRSTRLEN];
    char if_name[IFNAMSIZ] = {0}; // Buffer for interface name
    int broadcast_enable = 1;
    int pkt_info_enable = 1;
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

    struct iovec iov = { .iov_base = buffer, .iov_len = sizeof(buffer) };
    struct msghdr msg = {
        .msg_name = &client_addr,
        .msg_namelen = sizeof(client_addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };


    // Set server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Accept from any interface
    server_addr.sin_port = htons(PORT);

    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        perror("setsockopt(SO_BROADCAST) failed");
    }
    if (setsockopt(sockfd, IPPROTO_IP, IP_PKTINFO, &pkt_info_enable, sizeof(pkt_info_enable)) < 0) {
        perror("setsockopt IP_PKTINFO");
    }

    // Bind socket to the address
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Listening for UDP messages on port %d...\n", PORT);

    while (1) {
        ssize_t n = recvmsg(sockfd, &msg, 0);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }

        struct in_pktinfo *pktinfo = NULL;
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
                pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
                break;
            }
        }

        if (pktinfo != NULL) {
            if (if_indextoname(pktinfo->ipi_ifindex, if_name)) {
                printf("Packet received on interface: %s (index %d)\n", if_name, pktinfo->ipi_ifindex);
            } else {
                perror("if_indextoname failed");
            }
        }

        buffer[n] = '\0';  // Null-terminate
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);

        printf("Received message from %s:%d: %s\n",
               client_ip, ntohs(client_addr.sin_port), buffer);

        int value = 0;
        pthread_mutex_lock(&data.lock);
        value = data.value;
        pthread_mutex_unlock(&data.lock);

        if(has_ip_address(sockfd, if_name) == 0){
            printf("IP address is configured for interface %s, sending response on UDP unicast\n", if_name);
            // Send response
            if (sendto(sockfd, &value, sizeof(value), 0,
                       (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0) {
                perror("sendto (response) failed");
            } else {
                printf("Sent response to %s:%d\n",
                       client_ip, ntohs(client_addr.sin_port));
            }
        }
        // TODO send broadcast only to target interface
        else{
            printf("IP address is not configured for interface %s, sending response on UDP broadcast\n", if_name);
            // Enable broadcast        
            // Prepare broadcast address
            struct sockaddr_in broadcast_addr;
            memset(&broadcast_addr, 0, sizeof(broadcast_addr));
            broadcast_addr.sin_family = AF_INET;
            broadcast_addr.sin_port = client_addr.sin_port; // use same port as client
            broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST); // 255.255.255.255
        
            if (sendto(sockfd, &value, sizeof(value), 0,
                       (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
                perror("sendto (broadcast) failed");
            } else {
                printf("Sent broadcast response on port %d\n", ntohs(broadcast_addr.sin_port));
            }
        }
    }

    close(sockfd);
    return 0;
}
