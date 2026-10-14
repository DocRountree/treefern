# Claditor
Claditor is a GUI cladogram editor written in C++.

It stores cladograms as JSON files, and both tracks and commits changes to the file in real-time.

Claditor supports adding daughter/sister clades, editing clade names and information, and cutting/pasting clades onto new mother clades.




# Installation

Installing Claditor is quite straightforward.

1. Save the main.cpp file and CMakeLists.txt file in a directory of your choice.
2. Run the command:

   >cmake --build build -j

3. The program can now be started with the command:

   >./build/claditor
