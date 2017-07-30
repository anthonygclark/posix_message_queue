all:
	$(CXX) --std=c++11 test.cc mq.cc -o mq -lrt

clean:
	$(RM) mq
