# Compiler and flags
CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17
LDFLAGS = 

# Directories
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

# Files
TARGET = $(BIN_DIR)/in_memory_db

# Find all .cpp source files in src/
SRCS = $(wildcard $(SRC_DIR)/*.cpp)

# Generate corresponding .o object file names in obj/
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

# Default build rule
all: directories $(TARGET)

# Link final executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Create required directories if not exist
directories:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean build artifacts
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

# Run the program
run: all
	./$(TARGET)
