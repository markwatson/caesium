#include <Arduino.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <ntp.h>

#define UDP_PORT 4210 // Using 4210 for testing, change to 123 for NTP
#define BUFFER_SIZE 255

char UdpMessage[UDP_MESSAGE_SIZE] = "No UDP messages received yet.";
const char *getUdpMessage() { return UdpMessage; }

void UdpServerTask(void *pvParameters) {
  char rx_buffer[BUFFER_SIZE];
  char tx_buffer[] = "ACK";

  // 1. Create the socket
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    Serial.printf("Unable to create socket: errno %d\n", errno);
    vTaskDelete(NULL);
    return;
  }

  // 2. Bind the socket to the port
  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Listen on all interfaces
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(UDP_PORT);

  int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err < 0) {
    Serial.printf("Socket unable to bind: errno %d\n", errno);
    close(sock);
    vTaskDelete(NULL);
    return;
  }

  Serial.printf("UDP Server listening on port %d via FreeRTOS Task\n",
                UDP_PORT);

  // 3. The Loop (This runs independent of Arduino loop())
  while (true) {
    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    // BLOCKING CALL: The task sleeps here until data arrives.
    // This is the equivalent of "int packetSize = udp.parsePacket();" but
    // efficient.
    int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                       (struct sockaddr *)&source_addr, &socklen);

    // Error occurred during receiving
    if (len < 0) {
      Serial.printf("recvfrom failed: errno %d\n", errno);
      break;
    }
    // Data received
    else {
      // Null-terminate the buffer for safe printing
      rx_buffer[len] = 0;

      // Get the sender's IP as a C-string (equivalent to
      // udp.remoteIP().toString()) Note: inet_ntoa returns a static buffer, use
      // it immediately
      char *sender_ip = inet_ntoa(source_addr.sin_addr);

      // Get the sender's Port (equivalent to udp.remotePort())
      int sender_port = ntohs(source_addr.sin_port);

      // Print Logic
      Serial.printf("Received %d bytes from %s, port %d\n", len, sender_ip,
                    sender_port);
      Serial.printf("UDP packet contents: %s\n", rx_buffer);
      snprintf(UdpMessage, sizeof(UdpMessage),
               "Received UDP packet: %s, port: %d", rx_buffer, sender_port);

      // Send Reply (equivalent to beginPacket -> write -> endPacket)
      // We reuse 'source_addr' so we send it right back where it came from
      int err = sendto(sock, tx_buffer, strlen(tx_buffer), 0,
                       (struct sockaddr *)&source_addr, sizeof(source_addr));

      if (err < 0) {
        Serial.printf("Error occurred during sending: errno %d\n", errno);
      }
    }
  }

  vTaskDelete(NULL);
}
