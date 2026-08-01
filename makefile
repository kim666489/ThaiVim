args ?=

build:
	g++ -I ./src/include -o ./bin/thvim.out ./src/*.cpp -lncursesw
run:
	bin/thvim.out