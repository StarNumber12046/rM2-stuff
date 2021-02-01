#include "Commands.h"

#include <iostream>
#include <sstream>

// TODO: This is almost becoming a lisp interpreter, maybe move to one at some
// point?
namespace {

ErrorOr<std::function<void()>>
getCommandFn(Launcher& launcher, std::string_view command);

std::vector<std::string_view>
split(std::string_view str, std::string_view delims = " ") {
  std::vector<std::string_view> output;

  for (auto first = str.data(), second = str.data(), last = first + str.size();
       second != last && first != last;
       first = second + 1) {
    second =
      std::find_first_of(first, last, std::cbegin(delims), std::cend(delims));

    if (first != second) {
      output.emplace_back(first, second - first);
    }
  }

  return output;
}

ErrorOr<std::vector<std::string_view>>
tokenize(std::string_view str) {
  std::vector<std::string_view> output;

  bool inQuotes = false;
  for (auto first = str.data(), second = str.data(), last = first + str.size();
       first < last;
       ++second) {
    if (second == last && first != second) {
      output.emplace_back(first, second - first);
      break;
    }

    if (*second == ' ' && !inQuotes && first != second) {
      output.emplace_back(first, second - first);
      first = second + 1;
    } else if (*second == '"') {
      if (inQuotes) {
        inQuotes = false;
        output.emplace_back(first, second - first);
      } else {
        inQuotes = true;
      }
      first = second + 1;
    }
  }

  if (inQuotes) {
    return Error{ "Unclosed quotes" };
  }

  std::cout << "parsed: ";
  for (const auto& token : output) {
    std::cout << '"' << token << "\" ";
  }
  std::cout << std::endl;

  return output;
}

template<typename V>
ErrorOr<V>
parseArg(std::string_view arg) {
  static_assert(!std::is_same_v<V, V>, "No parser for type");
}

template<>
ErrorOr<std::string_view>
parseArg<std::string_view>(std::string_view arg) {
  return arg;
}

template<>
ErrorOr<std::string>
parseArg<std::string>(std::string_view arg) {
  return std::string(arg);
}

template<>
ErrorOr<ActionConfig>
parseArg<ActionConfig>(std::string_view arg) {
  using namespace rmlib::input;

  auto tokens = split(arg, ":");
  if (tokens.empty()) {
    return Error{ "Empty action" };
  }

  ActionConfig result;
  if (tokens.front() == "Swipe") {
    result.type = ActionConfig::Swipe;
    if (tokens.size() != 3) {
      return Error{ "Expected Swipe:direction:fingers" };
    }

    if (tokens[1] == "Up") {
      result.direction = SwipeGesture::Up;
    } else if (tokens[1] == "Down") {
      result.direction = SwipeGesture::Down;
    } else if (tokens[1] == "Left") {
      result.direction = SwipeGesture::Left;
    } else if (tokens[1] == "Right") {
      result.direction = SwipeGesture::Right;
    } else {
      return Error{ "Unknown direction: " + std::string(tokens[1]) };
    }

    auto num = std::string(tokens[2]);
    result.fingers = atoi(num.c_str());
  } else if (tokens.front() == "Pinch") {
    result.type = ActionConfig::Pinch;
    if (tokens.size() != 3) {
      return Error{ "Expected Pinch:direction:fingers" };
    }

    if (tokens[1] == "In") {
      result.direction = PinchGesture::In;
    } else if (tokens[1] == "Out") {
      result.direction = PinchGesture::Out;
    } else {
      return Error{ "Unknown direction: " + std::string(tokens[1]) };
    }

    auto num = std::string(tokens[2]);
    result.fingers = atoi(num.c_str());
  } else if (tokens.front() == "Tap") {
    result.type = ActionConfig::Tap;

    if (tokens.size() != 2) {
      return Error{ "Expected Tap:fingers" };
    }

    auto num = std::string(tokens[1]);
    result.fingers = atoi(num.c_str());
  } else {
    return Error{ "Unknown gesture: " + std::string(tokens.front()) };
  }

  return result;
}

template<typename... Args>
ErrorOr<std::tuple<Args...>>
anyError(ErrorOr<Args>... args) {
  bool hasError = (isError(args) || ... || false);
  if (!hasError) {
    return std::tuple{ *args... };
  }

  return Error{ ((isError(args) ? getError(args).msg + ", " : "") + ... + "") };
}

template<typename T>
struct ToOwningImpl {
  using type = T;
};

template<>
struct ToOwningImpl<std::string_view> {
  using type = std::string;
};

// Converts non owning types to owning types. Ex string_view to string.
// Used when curry-ing the commands for actions.
template<typename T>
using ToOwning = typename ToOwningImpl<T>::type;

struct Command {

  template<typename... Args>
  static ErrorOr<std::tuple<Args...>> parseArgs(
    const std::vector<std::string_view>& args) {
    if (args.size() != sizeof...(Args) + 1) {
      return Error{ "Invalid number of arguments for '" + std::string(args[0]) +
                    "', expected " + std::to_string(sizeof...(Args)) + " got " +
                    std::to_string(args.size() - 1) };
    }

    // Skip command name.
    auto argIt = std::next(args.begin());
    (void)argIt;

    // Parse each argument.
    auto argOrErrorTuple =
      std::tuple<ErrorOr<Args>...>{ parseArg<Args>(*argIt++)... };

    // Convert from tuple of errors to error of tuple.
    return std::apply([](auto&... args) { return anyError(args...); },
                      argOrErrorTuple);
  }

  template<typename... Args>
  constexpr Command(CommandResult (*fn)(Launcher&, Args...),
                    std::string_view help) noexcept
    : fn(reinterpret_cast<void*>(fn)), help(help) {
    cmdFn = [](auto* fn,
               Launcher& launcher,
               const std::vector<std::string_view>& args) -> CommandResult {
      auto argsOrErrors = parseArgs<Args...>(args);
      if (isError(argsOrErrors)) {
        return getError(argsOrErrors);
      }

      auto argTuple = std::tuple_cat(std::tuple<Launcher&>{ launcher },
                                     std::move(*argsOrErrors));
      auto* fnPtr = reinterpret_cast<CommandResult (*)(Launcher&, Args...)>(fn);
      return std::apply(fnPtr, argTuple);
    };

    parseFn = [](auto* fn,
                 Launcher& launcher,
                 const std::vector<std::string_view>& args)
      -> ErrorOr<std::function<CommandResult()>> {
      auto argsOrErrors = parseArgs<ToOwning<Args>...>(args);
      if (isError(argsOrErrors)) {
        return getError(argsOrErrors);
      }

      auto argTuple = std::tuple_cat(std::tuple<Launcher&>{ launcher },
                                     std::move(*argsOrErrors));
      return [fn, args = std::move(argTuple)]() {
        auto* fnPtr =
          reinterpret_cast<CommandResult (*)(Launcher&, Args...)>(fn);
        return std::apply(fnPtr, args);
      };
    };
  }

  CommandResult operator()(Launcher& launcher,
                           const std::vector<std::string_view>& args) const {
    return cmdFn(fn, launcher, args);
  }

  ErrorOr<std::function<CommandResult()>> parse(
    Launcher& launcher,
    const std::vector<std::string_view>& args) const {
    return parseFn(fn, launcher, args);
  }

  void* fn;
  std::string_view help;

  CommandResult (*cmdFn)(void*,
                         Launcher&,
                         const std::vector<std::string_view>&);

  ErrorOr<std::function<CommandResult()>> (
    *parseFn)(void*, Launcher&, const std::vector<std::string_view>&);
};

CommandResult
help(Launcher&);

CommandResult
launch(Launcher& launcher, std::string_view name) {
  auto* app = launcher.getApp(name);
  if (app == nullptr) {
    return Error{ "App not found " + std::string(name) };
  }

  launcher.switchApp(*app);

  return std::string("Launching: ") + std::string(name);
}

CommandResult
show(Launcher& launcher) {
  launcher.drawAppsLauncher();
  return "OK";
}

CommandResult
hide(Launcher& launcher) {
  launcher.closeLauncher();
  return "OK";
}

template<typename It>
It
getNext(It start, It begin, It end) {
  auto it = start;
  it++;
  while (it != start) {
    if (it == end) {
      it = begin;
    }

    if (it->isRunning()) {
      break;
    }

    it++;
  }

  return it;
}

CommandResult
switchTo(Launcher& launcher, std::string_view arg) {

  if (arg == "next") {
    auto start = std::find_if(
      launcher.apps.begin(), launcher.apps.end(), [&launcher](auto& app) {
        return app.description.path == launcher.currentAppPath;
      });
    if (launcher.currentAppPath.empty() || start == launcher.apps.end()) {
      return "No apps running";
    }

    auto it = getNext(start, launcher.apps.begin(), launcher.apps.end());
    launcher.switchApp(*it);
  } else if (arg == "prev") {
    auto start = std::find_if(
      launcher.apps.rbegin(), launcher.apps.rend(), [&launcher](auto& app) {
        return app.description.path == launcher.currentAppPath;
      });
    if (launcher.currentAppPath.empty() || start == launcher.apps.rend()) {
      return "No apps running";
    }

    auto it = getNext(start, launcher.apps.rbegin(), launcher.apps.rend());
    launcher.switchApp(*it);
  } else if (arg == "last") {
    auto* currentApp = launcher.getCurrentApp();
    App* lastApp = nullptr;
    for (auto& app : launcher.apps) {
      if (app.runInfo.has_value() && &app != currentApp &&
          (lastApp == nullptr || app.lastActivated > lastApp->lastActivated)) {
        lastApp = &app;
      }
    }

    if (lastApp == nullptr) {
      return "No other apps";
    }
    launcher.switchApp(*lastApp);
  } else {
    return Error{ "Unknown switch target, expected <next|prev|last>, got: " +
                  std::string(arg) };
  }

  return "OK";
}

CommandResult
onAction(Launcher& launcher, ActionConfig action, std::string_view command) {
  auto fnOrError = getCommandFn(launcher, command);
  if (isError(fnOrError)) {
    return Error{ "Can't add action: " + getError(fnOrError).msg +
                  " for command: \"" + std::string(command) + "\"" };
  }

  launcher.config.actions.emplace_back(Action{ action, *fnOrError });
  return "OK";
}

// clang-format off
const std::unordered_map<std::string_view, Command> commands = {
  { "help",   { help,   "- Show help" } },
  { "launch", { launch, "- launch <app name> - Start or switch to app" } },
  { "show",   { show,   "- Show the launcher" } },
  { "hide",   { hide,   "- Hide the launcher" } },
  { "switch", { switchTo,
  "- switch <next|prev|last> - Switch to the next, previous or last running app"
  } },
  { "on",     { onAction,
  "- on <gesture> <command> - execute command when the given action occurs"
  } },
};
// clang-format on

CommandResult
help(Launcher&) {
  std::stringstream ss;

  ss << "Commands:\n";

  for (const auto& [name, cmd] : commands) {
    ss << "\t" << name << " " << cmd.help << "\n";
  }

  return ss.str();
}

ErrorOr<std::function<void()>>
getCommandFn(Launcher& launcher, std::string_view command) {
  auto tokensOrErr = tokenize(command);
  if (isError(tokensOrErr)) {
    return getError(tokensOrErr);
  }

  const auto tokens = *tokensOrErr;
  if (tokens.empty()) {
    // Nothing to execute, doesn't fail.
    return Error{ "Empty command" };
  }

  auto cmdIt = commands.find(tokens.front());
  if (cmdIt == commands.end()) {
    return Error{ std::string("Command ") + std::string(tokens.front()) +
                  " not found" };
  }

  auto parsedFn = cmdIt->second.parse(launcher, tokens);
  if (isError(parsedFn)) {
    return getError(parsedFn);
  }

  return [fn = std::move(*parsedFn)]() {
    auto res = fn();
    if (isError(res)) {
      std::cerr << getError(res).msg << std::endl;
    }
  };
}

} // namespace

CommandResult
doCommand(Launcher& launcher, std::string_view command) {
  auto tokensOrErr = tokenize(command);
  if (isError(tokensOrErr)) {
    return getError(tokensOrErr);
  }

  const auto tokens = *tokensOrErr;
  if (tokens.empty()) {
    // Nothing to execute, doesn't fail.
    return "";
  }

  auto cmdIt = commands.find(tokens.front());
  if (cmdIt == commands.end()) {
    return Error{ std::string("Command ") + std::string(tokens.front()) +
                  " not found" };
  }

  return cmdIt->second(launcher, tokens);
}