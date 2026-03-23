Need to:
- [x] Resolder the pins
- [x] Test the uart implementation as is
- [x] Use new approach for tracking time w/ uart
- [x] Cleanup code
- [x] Use esp_timer_get_time()
- [ ] Update README
- [ ] Remove the complicated messaging sync system that triggers the time read from PPS. Just continually read the current time in a loop from the GPS, and let PPS interrupt.
- [ ] Tweak the NTP server to lower latency (there's a lower level API could use)
- [x] Baud Rate Squeezing: 38400 now, 460800 will lower latency from GPS. Need to probably change it on the GPS side though, so maybe not worth it. (EDIT: tried by connection not good enough)

