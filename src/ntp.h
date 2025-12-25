#ifndef NTP_H
#define NTP_H
#define UDP_MESSAGE_SIZE 1024

extern char UdpMessage[UDP_MESSAGE_SIZE];

const char *getUdpMessage();

/**
 * Task that responds to UDP packets with NTP time information.
 * Need to run it on a separate FreeRTOS task.
 */
void UdpServerTask(void *pvParameters);

#endif // NTP_H
