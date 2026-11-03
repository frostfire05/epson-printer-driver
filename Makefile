CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pedantic
INCLUDES := -I/usr/include/libusb-1.0
LIBS     := -lusb-1.0 -lpthread

TARGET   := epson_demo
SRCS     := epson_l130_driver.cpp print_spooler.cpp image_loader.cpp main_demo.cpp
OBJS     := $(SRCS:.cpp=.o)
HEADERS  := epson_l130_driver.hpp print_spooler.hpp driver_errors.hpp image_loader.hpp

.PHONY: all clean run pdf_output

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

pdf_output:
	mkdir -p pdf_output

run: $(TARGET) pdf_output
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf pdf_output
