/* (C) 2012-2022 by László Nagy
    This file is part of Bear.

    Bear is a tool to generate compilation database for clang tooling.

    Bear is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Bear is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Application.h"
#include "citnames/citnames-forward.h"
#include "intercept/intercept-forward.h"

#include <optional>

namespace {
    constexpr std::optional<std::string_view> ADVANCED_GROUP = {"advanced options"};
    constexpr std::optional<std::string_view> DEVELOPER_GROUP = {"developer options"};

}

namespace bear {

    Command::Command(rust::Result<ps::CommandPtr>&& intercept, rust::Result<ps::CommandPtr>&& citnames, fs::path output) noexcept
            : ps::Command()
            , intercept_(std::move(intercept))
            , citnames_(std::move(citnames))
            , output_(std::move(output))
    { }

    [[nodiscard]] rust::Result<int> Command::execute() const
    {
        if (intercept_.is_err()) {
            return rust::Err(intercept_.unwrap_err());
        }
        if (citnames_.is_err()) {
            return rust::Err(citnames_.unwrap_err());
        }

        std::error_code error_code;

        auto result = intercept_
                .and_then<int>([](const auto &cmd){
                    return cmd->execute();
                });
        if (fs::exists(output_, error_code)) {
            citnames_
                    .on_success([](const auto &cmd) {
                        return cmd->execute();
                    });
            fs::remove(output_, error_code);
        }
        return result;
    }

    Application::Application()
            : ps::ApplicationFromArgs(ps::ApplicationLogConfig("bear", "br"))
    { }

    rust::Result<flags::Arguments> Application::parse(int argc, const char **argv) const
    {
        const flags::Parser intercept_parser("intercept", cmd::VERSION, {
                {cmd::intercept::FLAG_OUTPUT,        {1,  false, "path of the result file",        {cmd::intercept::DEFAULT_OUTPUT}, std::nullopt}},
                {cmd::intercept::FLAG_FORCE_PRELOAD, {0,  false, "force to use library preload",   std::nullopt,                     DEVELOPER_GROUP}},
                {cmd::intercept::FLAG_FORCE_WRAPPER, {0,  false, "force to use compiler wrappers", std::nullopt,                     DEVELOPER_GROUP}},
                {cmd::intercept::FLAG_LIBRARY,       {1,  false, "path to the preload library",    {cmd::library::DEFAULT_PATH},     DEVELOPER_GROUP}},
                {cmd::intercept::FLAG_WRAPPER,       {1,  false, "path to the wrapper executable", {cmd::wrapper::DEFAULT_PATH},     DEVELOPER_GROUP}},
                {cmd::intercept::FLAG_WRAPPER_DIR,   {1,  false, "path to the wrapper directory",  {cmd::wrapper::DEFAULT_DIR_PATH}, DEVELOPER_GROUP}},
                {cmd::intercept::FLAG_COMMAND,       {-1, true,  "command to execute",             std::nullopt,                     std::nullopt}}
        });

        const flags::Parser citnames_parser("citnames", cmd::VERSION, {
                {cmd::citnames::FLAG_INPUT,      {1, false, "path of the input file",                    {cmd::intercept::DEFAULT_OUTPUT}, std::nullopt}},
                {cmd::citnames::FLAG_OUTPUT,     {1, false, "path of the result file",                   {cmd::citnames::DEFAULT_OUTPUT},  std::nullopt}},
                {cmd::citnames::FLAG_CONFIG,     {1, false, "path of the config file",                   std::nullopt,                     std::nullopt}},
                {cmd::citnames::FLAG_APPEND,     {0, false, "append to output, instead of overwrite it", std::nullopt,                     std::nullopt}},
                {cmd::citnames::FLAG_RUN_CHECKS, {0, false, "can run checks on the current host",        std::nullopt,                     std::nullopt}}
        });

        const flags::Parser parser("bear", cmd::VERSION, {intercept_parser, citnames_parser}, {
                {cmd::citnames::FLAG_OUTPUT,         {1,  false, "path of the result file",                  {cmd::citnames::DEFAULT_OUTPUT},  std::nullopt}},
                {cmd::citnames::FLAG_APPEND,         {0,  false, "append result to an existing output file", std::nullopt,                     ADVANCED_GROUP}},
                {cmd::citnames::FLAG_CONFIG,         {1,  false, "path of the config file",                  std::nullopt,                     ADVANCED_GROUP}},
                {cmd::intercept::FLAG_FORCE_PRELOAD, {0,  false, "force to use library preload",             std::nullopt,                     ADVANCED_GROUP}},
                {cmd::intercept::FLAG_FORCE_WRAPPER, {0,  false, "force to use compiler wrappers",           std::nullopt,                     ADVANCED_GROUP}},
                {cmd::bear::FLAG_BEAR,               {1,  false, "path to the bear executable",              {cmd::bear::DEFAULT_PATH},        DEVELOPER_GROUP}},
                {cmd::intercept::FLAG_LIBRARY,       {1,  false, "path to the preload library",              {cmd::library::DEFAULT_PATH},     DEVELOPER_GROUP}},
                {cmd::intercept::FLAG_WRAPPER,       {1,  false, "path to the wrapper executable",           {cmd::wrapper::DEFAULT_PATH},     DEVELOPER_GROUP}},
                {cmd::intercept::FLAG_WRAPPER_DIR,   {1,  false, "path to the wrapper directory",            {cmd::wrapper::DEFAULT_DIR_PATH}, DEVELOPER_GROUP}},
                {cmd::intercept::FLAG_COMMAND,       {-1, true,  "command to execute",                       std::nullopt,                     std::nullopt}}
        });
        return parser.parse_or_exit(argc, const_cast<const char **>(argv));
    }

    rust::Result<ps::CommandPtr> Application::command(const flags::Arguments &args) const
    {
        auto configuration = config::Configuration::load_config(args);

        return configuration
                .and_then<ps::CommandPtr>([this, &args](const config::Configuration& configuration) -> rust::Result<ps::CommandPtr> {
                    auto citnames = cs::Citnames(configuration.citnames, log_config_);
                    auto intercept = ic::Intercept(configuration.intercept, log_config_);

                    if (citnames.matches(args)) {
                        return citnames.subcommand(args);
                    }
                    if (intercept.matches(args)) {
                        return intercept.subcommand(args);
                    }
                    if (args.as_string(flags::COMMAND).is_ok()) {
                        return rust::Err(std::runtime_error("Invalid subcommand"));
                    }
                    
                    auto config = configuration;
                    config.citnames.output_file = args.as_string(cmd::citnames::FLAG_OUTPUT)
                            .unwrap_or(cmd::citnames::DEFAULT_OUTPUT);
                    config.citnames.input_file = config.citnames.output_file.replace_extension(".events.json");
                    config.intercept.output_file = config.citnames.input_file;

                    intercept.load_config(config.intercept);
                    auto intercept_cmd = intercept.subcommand(args);

                    citnames.load_config(config.citnames);
                    auto citnames_cmd = citnames.subcommand(args);

                    auto bear_command = std::make_unique<Command>(std::move(intercept_cmd), std::move(citnames_cmd), config.intercept.output_file);
                    return rust::Ok<ps::CommandPtr>(std::move(bear_command));
                });
    }
}
