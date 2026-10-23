# Host unit tests for the firmware's pure-C parts. Usage: make -C hardware/tests
SRC = ../hid_bridge/src
CFLAGS = -std=c11 -Wall -Wextra -g -fsanitize=address,undefined
CXXFLAGS = -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined

all: test test_hid_state test_hid_input test_video_write test_video_stream test_airplay_source
	./test
	./test_hid_state
	./test_hid_input
	./test_video_write
	./test_video_stream
	./test_airplay_source

test: test_main.c $(SRC)/bplist/bplist.c $(SRC)/video/gop.c $(SRC)/hid/command.c
	$(CC) $(CFLAGS) -o $@ $^

test_hid_state: test_hid_state.cpp $(SRC)/hid/state.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_hid_input: test_hid_input.cpp $(SRC)/hid/serial_line.h $(SRC)/hid/report_send.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_video_write: test_video_write.cpp $(SRC)/video/write_deadline.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_video_stream: test_video_stream.cpp $(SRC)/video/stream_control.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_airplay_source: test_airplay_source.c $(SRC)/airplay/source_state.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f test test_hid_state test_hid_input test_video_write test_video_stream test_airplay_source

.PHONY: all clean
