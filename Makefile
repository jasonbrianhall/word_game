CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LIBS     := $(shell sdl2-config --cflags --libs) -lSDL2_ttf
WORDLIST ?= /usr/share/dict/linux.words

letterlock: letterlock.cpp words.h answers.h DejaVuMono.h
	$(CXX) $(CXXFLAGS) letterlock.cpp -o $@ $(LIBS)

# words.h and answers.h ship pre-built, so a plain build (and CI) never needs the
# word list. Regenerate with:  make words     (needs the 'words' package)
#                              make answers   (also needs: pip install wordfreq)
words:
	sh gen_words.sh $(WORDLIST) > words.h.tmp && mv words.h.tmp words.h

answers:
	python3 gen_answers.py $(WORDLIST) > answers.h.tmp && mv answers.h.tmp answers.h

clean:
	rm -f letterlock words.h.tmp answers.h.tmp

.PHONY: clean words answers
