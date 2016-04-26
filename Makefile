
#Debug
#CFLAGS=-g -Wall
#Prod
CFLAGS=-Os -fwhole-program

kwuartboot: kwuartboot.o
	$(CC) $(CFLAGS) -o kwuartboot kwuartboot.o

clobber: clean
	-$(RM) -f kwuartboot

clean:
	-$(RM) -f kwuartboot.o
