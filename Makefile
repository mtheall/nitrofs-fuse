CFLAGS  := -g -Wall `pkg-config --cflags fuse` -DFUSE_USE_VERSION=26
LDFLAGS := `pkg-config --libs fuse`

all: nitrofs