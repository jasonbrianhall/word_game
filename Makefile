CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LIBS     := $(shell sdl2-config --cflags --libs) -lSDL2_ttf
WORDLIST ?= /usr/share/dict/linux.words

wordle: wordle.cpp words.h DejaVuMono.h
	$(CXX) $(CXXFLAGS) wordle.cpp -o $@ $(LIBS)

words.h: gen_words.sh
	sh gen_words.sh $(WORDLIST) > $@.tmp && mv $@.tmp $@

clean:
	rm -f wordle words.h words.h.tmp

.PHONY: clean
