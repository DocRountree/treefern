# TreeFern
TreeFern is a lightweight FOSS tool for editing cladograms, written in C++.

It stores cladograms as JSON files, and both tracks changes made in the file and commits changes made in the GUI in real-time, and saves them automatically. 

TreeFern supports adding daughter/sister clades, editing clade names and information, and cutting/pasting clades onto new mother clades.




# Installation

Installing TreeFern is quite straightforward.

1. Save the [main.cpp](main.cpp) file and [CMakeLists.txt](CMakeLists.txt) file in a directory of your choice. Make sure you have CMake installed and that your machine can compile C++ code.
2. In that directory, to build the program, run the command:

   >cmake -B build
   
3. To compile the program, run the command:

   >cmake --build build -j

4. TreeFern can now be started with the command:

   >./build/treefern


# Usage

TreeFern reads .JSON files to build a recursive cladogram, and stores written information about clades according to user preference. (See the [JSON Template](template.json) for more information.)

<img width="1667" height="1312" alt="2" src="https://github.com/user-attachments/assets/7670062f-9244-49b9-b407-100d8e7eb7c3" />


Functionality can be accessed via keyboard shortcuts, or via GUI buttons in the side panel which appears when a clade is selected by a single left-click.

<img width="1611" height="1369" alt="1" src="https://github.com/user-attachments/assets/3ea700d6-0341-42e3-a45e-72970173b55e" />


Double-clicking a clade opens the Edit menu (also accessible by clicking "Edit" in the side-panel, or by the keyboard shortcut 'e').

<img width="1210" height="1097" alt="3" src="https://github.com/user-attachments/assets/57944cc9-2241-46ac-8652-a641fcf9be43" />
