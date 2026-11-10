mangle: main.c
	cc -std=c90 $(if $(DEBUG),-fsanitize=address -g,-O2) main.c
