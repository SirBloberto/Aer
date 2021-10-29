SOURCE := $(foreach file, source, $(wildcard $(addprefix $(file)/*, .c*)))
OBJECT := $(addprefix object/, $(addsuffix .o, $(notdir $(basename $(SOURCE)))))

Compiler: $(OBJECT)
	gcc -g -o binary/Compiler $^

object/%.o: source/%.c
	gcc -g -c $< -o $@

Clean:
	rm -rf binary/* object/*