cmake_minimum_required (VERSION 3.8)
project(libuev LANGUAGES C)

set(uev_sources
	src/cron.c
	src/event.c
	src/io.c
	src/signal.c
	src/timer.c
	src/uev.c)

set(uev_cflags
	-D_GNU_SOURCE 
	-D_TIME_BITS=64
	-W -Wall -Wextra 
	-std=gnu11)

add_library(uev STATIC ${uev_sources})
target_compile_definitions(uev PRIVATE)
target_compile_options(uev PRIVATE ${uev_cflags})
target_include_directories(uev PUBLIC src)