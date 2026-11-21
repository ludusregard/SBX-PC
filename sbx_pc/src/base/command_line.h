// Taking our settings off the command line before ReXGlue reads it
// (main.cpp). On Windows the SDK splits the process's command line string
// itself, so the arguments are removed from that string where they stand:
// these split it as Windows does (CommandLineToArgvW's rules) and remember
// where each argument was.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sbx::command_line {

// One argument: its text, and where it stood in the line ([begin, end)).
template <typename Char>
struct Token {
  std::basic_string<Char> text;
  size_t begin = 0;
  size_t end = 0;
};

// Splits a command line as CommandLineToArgvW does. The program name (the
// first argument) is read up to the next quote if it starts with one, or
// to the next space; the rest follow the backslash and quote rules: 2n
// backslashes before a quote are n backslashes and the quote opens or
// closes a quoted part, 2n+1 are n and a literal quote, and backslashes
// before anything else are kept as they are.
template <typename Char>
std::vector<Token<Char>> Split(std::basic_string_view<Char> line) {
  auto is_space = [](Char c) { return c == Char(' ') || c == Char('\t'); };
  std::vector<Token<Char>> tokens;
  size_t i = 0;

  // The program name.
  if (i < line.size()) {
    Token<Char> program;
    program.begin = i;
    if (line[i] == Char('"')) {
      i++;
      while (i < line.size() && line[i] != Char('"')) {
        program.text += line[i++];
      }
      if (i < line.size()) {
        i++;  // the closing quote
      }
    } else {
      while (i < line.size() && !is_space(line[i])) {
        program.text += line[i++];
      }
    }
    program.end = i;
    tokens.push_back(std::move(program));
  }

  while (true) {
    while (i < line.size() && is_space(line[i])) {
      i++;
    }
    if (i >= line.size()) {
      break;
    }
    Token<Char> token;
    token.begin = i;
    bool quoted = false;
    while (i < line.size() && (quoted || !is_space(line[i]))) {
      if (line[i] == Char('\\')) {
        size_t backslashes = 0;
        while (i < line.size() && line[i] == Char('\\')) {
          backslashes++;
          i++;
        }
        if (i < line.size() && line[i] == Char('"')) {
          token.text.append(backslashes / 2, Char('\\'));
          if (backslashes % 2) {
            token.text += Char('"');
            i++;
          }
          // An even count leaves the quote for the next pass to open or close.
        } else {
          token.text.append(backslashes, Char('\\'));
        }
        continue;
      }
      if (line[i] == Char('"')) {
        if (quoted && i + 1 < line.size() && line[i + 1] == Char('"')) {
          token.text += Char('"');  // "" inside quotes: a literal quote
          i += 2;
          continue;
        }
        quoted = !quoted;
        i++;
        continue;
      }
      token.text += line[i++];
    }
    token.end = i;
    tokens.push_back(std::move(token));
  }
  return tokens;
}

// The line without the arguments at `positions` (sorted, none of them 0):
// each is cut together with the spaces before it, so the rest of the line
// stays exactly as it was. The result is never longer than the line.
template <typename Char>
std::basic_string<Char> Remove(std::basic_string_view<Char> line,
                               const std::vector<Token<Char>>& tokens,
                               const std::vector<size_t>& positions) {
  std::basic_string<Char> result(line);
  for (auto it = positions.rbegin(); it != positions.rend(); ++it) {
    size_t position = *it;
    if (position == 0 || position >= tokens.size()) {
      continue;
    }
    size_t from = tokens[position - 1].end;
    size_t to = tokens[position].end;
    result.erase(from, to - from);
  }
  return result;
}

}  // namespace sbx::command_line
