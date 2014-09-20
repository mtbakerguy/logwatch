LUAPATH=../lua

INCLUDE=-I$(LUAPATH)/include
LIBS=-L$(LUAPATH)/lib -llua
CXX=g++ -Wall -g $(INCLUDE) $(LIBS) 
CC=$(CXX)
all: logwatch
        
logwatch: logwatch.o
        $(CXX) $< -o $@ $(LIBS)

logwatch.o: logwatch.cpp

clean:
        rm -fr logwatch logwatch.o *~
