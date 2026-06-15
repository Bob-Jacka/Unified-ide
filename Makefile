TARGET_NAME = unified_editor

build: main.c
	$(CC) main.c -o $(TARGET_NAME) -Wall -Wextra -pedantic -std=c23
	
clean:
	rm -f $(TARGET_NAME)
