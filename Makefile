SOURCE := $(foreach file, source, $(wildcard $(addprefix $(file)/*, .c*)))
OBJECT := $(addprefix object/, $(addsuffix .o, $(notdir $(basename $(SOURCE)))))

Compiler: $(OBJECT)
	gcc -o binary/Compiler $^

object/%.o: source/%.c
	gcc -c $< -o $@

Clean:
	rm -rf binary/* object/*