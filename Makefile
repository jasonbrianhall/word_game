CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LIBS     := $(shell sdl2-config --cflags --libs) -lSDL2_ttf
WORDLIST ?= /usr/share/dict/linux.words

wordle: wordle.cpp words.h answers.h DejaVuMono.h
	$(CXX) $(CXXFLAGS) wordle.cpp -o $@ $(LIBS)

words.h: gen_words.sh
	sh gen_words.sh $(WORDLIST) > $@.tmp && mv $@.tmp $@

# answers.h ships pre-built; regenerate with: make answers  (needs: pip install wordfreq)
answers:
	python3 gen_answers.py $(WORDLIST) > answers.h.tmp && mv answers.h.tmp answers.h

clean:
	rm -f wordle words.h words.h.tmp

.PHONY: clean answers
