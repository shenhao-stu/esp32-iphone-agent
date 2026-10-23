# Host unit tests for the firmware's pure-C parts. Usage: make -C hardware/tests
SRC = ../hid_bridge/src
CFLAGS = -std=c11 -Wall -Wextra -g -fsanitize=address,undefined
CXXFLAGS = -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined

all: test test_hid_state test_video_write
	./test
	./test_hid_state
	./test_video_write

test: test_main.c $(SRC)/bplist/bplist.c $(SRC)/video/gop.c $(SRC)/hid/command.c
	$(CC) $(CFLAGS) -o $@ $^

test_hid_state: test_hid_state.cpp $(SRC)/hid/state.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test_video_write: test_video_write.cpp $(SRC)/video/write_deadline.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f test test_hid_state test_video_write

.PHONY: all clean
