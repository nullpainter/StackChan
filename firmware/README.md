## Matt's StackChan 

This fork provides small enhancements to AI features in the stock firmware.

### Interaction

* Tap head to interrupt — single tap while she's talking cuts her off mid-sentence but stays in the conversation.
* Double-tap head to leave chat — two taps exits conversation mode.


### LEDs

* Listening — solid colour on both strips.
* Speaking effect — blue pixel animation with random highlights, modulated by speech. 

Both options configurable via settings.


### Gaze and movement

* Idle movement option repurposed to movement during AI chat

### Display

* Chat speech bubbles disabled

### Ambient light

* Screen turns off in a dark room and back on once the room is lit again.
* Tapping a dark screen turns it on early, and keeps it on until the light returns.
* Can be toggled through settings.



## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```
