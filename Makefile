# use pkg-config for getting CFLAGS and LDLIBS
FFMPEG_LIBS=    libavdevice                        \
                libavformat                        \
                libavfilter                        \
                libavcodec                         \
                libswresample                      \
                libswscale                         \
                libavutil                          \

CFLAGS += -Wall -g -std=c11
CFLAGS := $(shell pkg-config --cflags $(FFMPEG_LIBS)) $(CFLAGS)
LDLIBS := $(shell pkg-config --libs $(FFMPEG_LIBS)) $(LDLIBS)

EXAMPLES=       tutorial01                       \
                tutorial02                       \
                tutorial03                       \
                tutorial04                       \
                tutorial05                       \
                tutorial06                       \
                tutorial07                       \

OBJS=$(addsuffix .o,$(EXAMPLES))

# the following examples make explicit use of the math library
tutorial01:           LDLIBS += -lm
tutorial02:           LDLIBS += -lm
tutorial03:           LDLIBS += -lm
tutorial04:           LDLIBS += -lm
tutorial05:           LDLIBS += -lm
tutorial06:           LDLIBS += -lm
tutorial07:           LDLIBS += -lm


.phony: all clean

all: $(OBJS) $(EXAMPLES)

clean:
	$(RM) $(EXAMPLES) $(OBJS)
