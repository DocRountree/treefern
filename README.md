# TreeFern
TreeFern is a lightweight FOSS tool for editing cladograms, written in C++ and based mostly on Qt.

It stores cladograms as JSON files, with live file synchronization and autosave. 

TreeFern supports adding daughter/sister clades, adding metadata information to clades, and cutting/pasting clades onto new mother clades.


# Features

* **Two View Layouts**
  * **Standard** — Tapered branching tree structure in which heavier child branches curve away from their parent branches, inspired by OneZoom.org's viewer.
  * **Disks** — Nested circular packing where child clades are inscribed discs inside their parent clade discs.

* **Dynamic Labels:** Labels too small to be seen are hidden until zoomed in, and labels dynamically adjust positions to avoid overcrowding. This is the most finicky feature and I will probably be working on it for a while. Label modes can also be toggled between an over-node and beside-node layout, which persists between sessions.

* **Dark & Light Modes:** Dark Mode is standard, and light mode can be activated in the view panel — setting persists across sessions.

* **Search & Navigation:** Navigation can be done entirely by keyboard or entirely by mouse, and clades can be found by a search bar in the side panel.
<br>
# Installation

To install TreeFern, use the following steps:

1. Save the [main.cpp](main.cpp) file and [CMakeLists.txt](CMakeLists.txt) file in a directory of your choice. Make sure you have CMake installed and that your machine can compile C++ code.
2. In that directory, to build the program, run the command:

   >cmake -B build
   
3. To compile the program, run the command:

   >cmake --build build -j

4. TreeFern can now be started with the command:

   >./build/treefern

# Usage

Treefern can be launched from the terminal by running:

>./treefern

Or by clicking in the GUI on the executable file (or a shortcut to it) that gets built during the installation.

In the terminal, a specific file can be opened with TreeFern by running:

>./treefern file.json

The GUI for the program also has a file picker, and can open JSON files that are dragged into the program canvas.

# Screenshots
<br>

**Standard vs. Disks layout:**

<img width="1705" height="1414" alt="Standard layout" src="https://github.com/user-attachments/assets/124aef28-8f36-4c05-a570-cf27de30c690" />
<img width="1717" height="1420" alt="Disks layout" src="https://github.com/user-attachments/assets/787e1073-e624-46f7-ae94-a4d7b69feb1d" />
<br>

**Light Mode:**

<img width="1720" height="1421" alt="Light mode" src="https://github.com/user-attachments/assets/7cbe0f3a-47f7-4e0a-9221-42b328ce9440" />
<br>

**Labels beside nodes:**

<img width="1718" height="1421" alt="Labels beside nodes" src="https://github.com/user-attachments/assets/e501e873-f8ca-4104-8ab2-2d8eace6a696" />
