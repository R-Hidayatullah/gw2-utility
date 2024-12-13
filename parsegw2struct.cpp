#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <string>
#include <algorithm>
#include <cstdlib>

// Helper function to create directories
void createDirectories(const std::string &path)
{
#ifdef _WIN32
    std::string command = "mkdir \"" + path + "\" > nul 2>&1";
#else
    std::string command = "mkdir -p \"" + path + "\"";
#endif
    system(command.c_str());
}

// Helper function to convert a string to lowercase
std::string toLowercase(const std::string &str)
{
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

void processFile(const std::string &inputFile, const std::string &outputDir)
{
    std::ifstream input(inputFile);
    if (!input.is_open())
    {
        std::cerr << "Error opening file: " << inputFile << "\n";
        return;
    }

    std::string line;
    std::string previousLine; // Variable to hold the previous line
    std::string currentChunk;
    int currentVersion = -1;
    std::ostringstream contentBuffer;
    std::regex chunkRegex(R"(Chunk:\s+(\w+))");
    std::regex versionRegex(R"(\/\* Version:\s+(\d+),)");

    while (std::getline(input, line))
    {
        std::smatch match;

        // If we find a chunk, save the previous line (if any) and the current line
        if (std::regex_search(line, match, chunkRegex))
        {
            // If there is content for the previous chunk, save it
            if (!currentChunk.empty())
            {
                std::string bufferContent = contentBuffer.str();

                // Check if the last line is the special comment and remove it
                if (bufferContent.find("/* ===============================================") != std::string::npos)
                {
                    size_t pos = bufferContent.rfind("\n/* ===============================================");
                    if (pos != std::string::npos)
                    {
                        bufferContent = bufferContent.substr(0, pos); // Remove the comment line
                    }
                }

                std::string fileName = outputDir + "/" + currentChunk + "/" + toLowercase(currentChunk) + ".h";
                createDirectories(outputDir + "/" + currentChunk);
                std::ofstream outFile(fileName);
                if (!outFile.is_open())
                {
                    std::cerr << "Error creating file: " << fileName << "\n";
                    continue;
                }

                // Add include guard
                std::string guardName = currentChunk;
                std::transform(guardName.begin(), guardName.end(), guardName.begin(), ::toupper);
                std::replace(guardName.begin(), guardName.end(), '.', '_');

                outFile << "#ifndef " << guardName << "_H\n";
                outFile << "#define " << guardName << "_H\n\n";
                outFile << "#include \"gw2format/gw2_type.h\"\n\n";
                outFile << bufferContent; // Write the cleaned buffer content
                outFile << "\n#endif // " << guardName << "_H\n";

                outFile.close();
            }

            // Start new chunk with the previous line
            currentChunk = match[1];
            currentVersion = -1;   // Reset version for new chunk
            contentBuffer.str(""); // Clear the buffer
            contentBuffer.clear();

            if (!previousLine.empty())
            {
                contentBuffer << previousLine << "\n"; // Add the previous line before the chunk
            }
        }

        // Detect Version
        if (std::regex_search(line, match, versionRegex))
        {
            currentVersion = std::stoi(match[1]); // Update the version
        }

        // Append current line to content buffer
        contentBuffer << line << "\n";

        // Save the current line as the previous line for the next iteration
        previousLine = line;
    }

    // Write the last chunk
    if (!currentChunk.empty())
    {
        std::string bufferContent = contentBuffer.str();

        // Check if the last line is the special comment and remove it
        if (bufferContent.find("/* ===============================================") != std::string::npos)
        {
            size_t pos = bufferContent.rfind("\n/* ===============================================");
            if (pos != std::string::npos)
            {
                bufferContent = bufferContent.substr(0, pos); // Remove the comment line
            }
        }

        std::string fileName = outputDir + "/" + currentChunk + "/" + toLowercase(currentChunk) + ".h";
        createDirectories(outputDir + "/" + currentChunk);
        std::ofstream outFile(fileName);
        if (!outFile.is_open())
        {
            std::cerr << "Error creating file: " << fileName << "\n";
        }
        else
        {
            // Add include guard
            std::string guardName = currentChunk;
            std::transform(guardName.begin(), guardName.end(), guardName.begin(), ::toupper);
            std::replace(guardName.begin(), guardName.end(), '.', '_');

            outFile << "#ifndef " << guardName << "_H\n";
            outFile << "#define " << guardName << "_H\n\n";
            outFile << "#include \"gw2format/gw2_type.h\"\n\n";
            outFile << bufferContent; // Write the cleaned buffer content
            outFile << "\n#endif // " << guardName << "_H\n";

            outFile.close();
        }
    }

    input.close();
}

int main()
{
    std::string inputFile = "input.txt"; // Path to your input file
    std::string outputDir = "fourcc";    // Directory to save generated files
    std::cout << "Started Parsing....\n";
    processFile(inputFile, outputDir);
    std::cout << "Finished\n";

    return 0;
}