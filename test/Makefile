sources=$(wildcard */*.c)
executables=$(patsubst %.c,%.exe,$(sources))

all: $(executables)

%.exe: %.c
	$(CC) -o $@ $^

testac056/testac056.exe: testac056/testac056.c
	$(CC) -pthread -o $@ $^

jhh1/jhh1.exe: jhh1/jhh1.c
	$(CC) -pthread -o $@ $^
