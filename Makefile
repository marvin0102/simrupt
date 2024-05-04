NAME = tttkml
tttkml-objs = simrupt.o game.o mcts.o 
obj-m := $(NAME).o 

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

GIT_HOOKS := .git/hooks/applied
all: $(GIT_HOOKS) kml ttt

kml: simrupt.c
	$(MAKE) -C $(KDIR) M=$(PWD) modules


ttt: ttt.c
	$(CC) -o $@ $^ $(CFLAGS) 

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo


clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf *.o *.d
	rm ttt
