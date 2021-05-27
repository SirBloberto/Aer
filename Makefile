CC := gcc
CFLAGS := -Wall

SOURCE := $(foreach file, source, $(wildcard $(addprefix $(file)/*, .c*)))
OBJECT := $(addprefix object/, $(addsuffix .o, $(notdir $(basename $(SOURCE)))))

Compiler: $(OBJECT)
	$(CC) -o binary/Compiler $^

object/%.o: source/%.c
	$(CC) $(CFLAGS) -c $< -o $@

Clean:
	@rm -rf binary/* object/*