sources=$(wildcard */*.c)
executables=$(patsubst %.c,%.exe,$(sources))

all: $(executables)

%.exe: %.c
	$(CC) -pthread -o $@ $^

testac056/testac056.exe: testac056/testac056.c
	$(CC) -pthread -o $@ $^

testcase5/testcase5.exe: testcase5/testcase5.c
	$(CC) -pthread -o $@ $^

testcase14/testcase14.exe: testcase14/testcase14.c
	$(CC) -pthread -o $@ $^

jhh1/jhh1.exe: jhh1/jhh1.c
	$(CC) -pthread -o $@ $^

.PHONY: clean

clean:
	$(RM) */*.exe
