LCC = /c/Users/frank/gbdk/bin/lcc
TARGET = snake.gb

all: $(TARGET)

$(TARGET): main.c
	$(LCC) -Wm-yn"SNAKE" -o $(TARGET) main.c

clean:
	rm -f *.gb *.o *.lst *.map *.sym
