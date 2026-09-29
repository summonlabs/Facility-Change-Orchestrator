#include <iostream>
#include <string>
#include <vector>

#include "fco/cli.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0u);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  std::string output;
  std::string error_output;
  const int code = fco::cli::run(arguments, output, error_output);

  if (!output.empty()) {
    std::cout << output;
    if (output.back() != '\n') std::cout << '\n';
  }
  if (!error_output.empty()) {
    std::cerr << error_output;
    if (error_output.back() != '\n') std::cerr << '\n';
  }
  std::cout.flush();
  std::cerr.flush();
  return code;
}
