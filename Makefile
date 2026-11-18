CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread
LDFLAGS ?= -pthread

SRCS := $(wildcard src/*.cpp)
OBJS := $(SRCS:.cpp=.o)
HDRS := $(wildcard src/*.hpp)

.PHONY: all clean install test

all: jelll

jelll: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

src/%.o: src/%.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

install: jelll
	install -d $(DESTDIR)/usr/local/bin
	install -m 0755 jelll $(DESTDIR)/usr/local/bin/jelll

test: jelll
	cd tests && python3 -m pytest -q

clean:
	rm -f $(OBJS) jelll
