// tmacpower - Live Mac power telemetry.
//
// Copyright (c) 2026 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

#include "CommandLineParser.hpp"
#include "Tui.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

/// One sample of instantaneous battery and adapter telemetry; watts are W.
struct PowerSample
{
    /// True when an external power source is connected.
    bool externalConnected = false;
    /// True when the battery controller reports charging.
    bool charging = false;
    /// Power entering the machine from the external supply.
    std::optional<double> supplyWatts;
    /// Power currently consumed by the system.
    std::optional<double> machineWatts;
    /// Power flowing into the battery, with discharge represented separately.
    std::optional<double> batteryChargeWatts;
    /// Power flowing out of the battery.
    std::optional<double> batteryDischargeWatts;
    /// Rated or negotiated adapter capacity, distinct from actual draw.
    std::optional<double> adapterWatts;
    /// Battery state of charge, in percent.
    std::optional<int64_t> chargePercent;
    /// Battery pack voltage, in V.
    std::optional<double> batteryVolts;
    /// Signed battery current, positive for charging, in A.
    std::optional<double> batteryAmps;
};

/// Return a dictionary member when it has the expected type.
CFTypeRef member(CFDictionaryRef dict, const char* key, CFTypeID type)
{
    if (!dict) return nullptr;
    const CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!cfKey) return nullptr;
    const CFTypeRef value = CFDictionaryGetValue(dict, cfKey);
    CFRelease(cfKey);
    return value && CFGetTypeID(value) == type ? value : nullptr;
}

/// Read a signed 64-bit integer, including negative battery current values.
std::optional<int64_t> number(CFDictionaryRef dict, const char* key)
{
    const CFTypeRef value = member(dict, key, CFNumberGetTypeID());
    if (!value) return std::nullopt;
    int64_t result = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &result);
    return result;
}

/// Read a nested dictionary without taking ownership of it.
CFDictionaryRef dictionary(CFDictionaryRef dict, const char* key)
{
    return static_cast<CFDictionaryRef>(member(dict, key, CFDictionaryGetTypeID()));
}

/// Read a Boolean registry value, or return false if absent.
bool boolean(CFDictionaryRef dict, const char* key)
{
    const CFTypeRef value = member(dict, key, CFBooleanGetTypeID());
    return value && CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

/// Convert a signed milliwatt reading to watts when present.
std::optional<double> milliwatts(CFDictionaryRef dict, const char* key)
{
    if (const auto value = number(dict, key)) return static_cast<double>(*value) / 1000.0;
    return std::nullopt;
}

/// Query the AppleSmartBattery registry service and adapter capacity.
PowerSample readPower()
{
    const io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"));
    if (service == IO_OBJECT_NULL)
        throw std::runtime_error("AppleSmartBattery was not found; this tool requires a Mac with a built-in battery.");

    CFMutableDictionaryRef properties = nullptr;
    const kern_return_t status = IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0);
    IOObjectRelease(service);
    if (status != KERN_SUCCESS || !properties)
        throw std::runtime_error("Could not read AppleSmartBattery telemetry from IOKit.");

    PowerSample sample;
    sample.externalConnected = boolean(properties, "ExternalConnected");
    sample.charging = boolean(properties, "IsCharging");
    sample.chargePercent = number(properties, "CurrentCapacity");
    if (const auto mv = number(properties, "Voltage")) sample.batteryVolts = static_cast<double>(*mv) / 1000.0;
    if (const auto ma = number(properties, "Amperage")) sample.batteryAmps = static_cast<double>(*ma) / 1000.0;

    const CFDictionaryRef telemetry = dictionary(properties, "PowerTelemetryData");
    sample.machineWatts = milliwatts(telemetry, "SystemLoad");
    sample.supplyWatts = milliwatts(telemetry, "SystemPowerIn");
    auto batteryPower = milliwatts(telemetry, "BatteryPower");
    if (!batteryPower && sample.batteryVolts && sample.batteryAmps)
        batteryPower = *sample.batteryVolts * *sample.batteryAmps;
    if (batteryPower)
    {
        sample.batteryChargeWatts = std::max(0.0, *batteryPower);
        sample.batteryDischargeWatts = std::max(0.0, -*batteryPower);
    }
    if (!sample.externalConnected) sample.supplyWatts = 0.0;

    const CFDictionaryRef adapter = dictionary(properties, "AdapterDetails");
    if (const auto watts = number(adapter, kIOPSPowerAdapterWattsKey))
        sample.adapterWatts = static_cast<double>(*watts);
    CFRelease(properties);

    if (sample.externalConnected && !sample.adapterWatts)
    {
        if (const CFDictionaryRef details = IOPSCopyExternalPowerAdapterDetails())
        {
            if (const auto watts = number(details, kIOPSPowerAdapterWattsKey))
                sample.adapterWatts = static_cast<double>(*watts);
            CFRelease(details);
        }
    }
    return sample;
}

/// Format an optional measured number with a fixed precision.
std::string formatValue(std::optional<double> value, int precision = 1)
{
    if (!value) return "unavailable";
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << *value;
    return out.str();
}

/// Format a battery percentage if reported by the controller.
std::string formatPercent(std::optional<int64_t> percent)
{
    return percent ? std::to_string(*percent) : "unavailable";
}

/// One table row, with its unit kept separate for column alignment.
struct TableRow
{
    /// Name of the measured or reported quantity.
    std::string label;
    /// Formatted reading, or an unavailable marker.
    std::string value;
    /// Unit of the reading, empty for textual states.
    std::string unit;
};

/// Build the rows shared by plain-text and interactive rendering.
std::vector<std::string> rows(const PowerSample& sample)
{
    const std::vector<TableRow> data = {
        {"Power source", sample.externalConnected ? "External" : "Battery", ""},
        {"Supply delivered", formatValue(sample.supplyWatts), "W"},
        {"Machine consumed", formatValue(sample.machineWatts), "W"},
        {"Battery charging", formatValue(sample.batteryChargeWatts), "W"},
        {"Battery discharging", formatValue(sample.batteryDischargeWatts), "W"},
        {"Adapter capacity", formatValue(sample.adapterWatts), "W"},
        {"Battery charge", formatPercent(sample.chargePercent), "%"},
        {"Battery voltage", formatValue(sample.batteryVolts, 2), "V"},
        {"Battery current", formatValue(sample.batteryAmps, 2), "A"},
        {"Charging state", sample.charging ? "Charging" : "Not charging", ""}
    };
    size_t labelWidth = std::string("Metric").size();
    size_t valueWidth = std::string("Value").size();
    for (const auto& row : data)
    {
        labelWidth = std::max(labelWidth, row.label.size());
        valueWidth = std::max(valueWidth, row.value.size());
    }
    std::vector<std::string> result;
    auto line = [labelWidth, valueWidth](const std::string& label, const std::string& value, const std::string& unit)
    {
        std::ostringstream out;
        out << std::left << std::setw(static_cast<int>(labelWidth)) << label << "  "
            << std::right << std::setw(static_cast<int>(valueWidth)) << value;
        if (!unit.empty()) out << "  " << unit;
        return out.str();
    };
    result.push_back(line("Metric", "Value", "Unit"));
    result.emplace_back(labelWidth + valueWidth + 8, '-');
    for (const auto& row : data) result.push_back(line(row.label, row.value, row.unit));
    return result;
}

/// Draw one sample within the current terminal bounds.
void draw(const PowerSample& sample)
{
    const size_t width = ut1::tui::terminalWidth();
    const size_t height = ut1::tui::terminalHeight();
    const auto data = rows(sample);
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_r(&now, &localTime);
    char clockText[16]{};
    std::strftime(clockText, sizeof(clockText), "%H:%M:%S", &localTime);
    size_t printed = 0;
    auto printLine = [width, height, &printed](const std::string& line, const char* color)
    {
        if (printed >= height) return;
        ut1::tui::printColoredLine(std::cout, line, width, color);
        ++printed;
    };
    ut1::tui::clearScreen();
    printLine(std::string(" tmacpower  |  live power  |  ") + clockText + " ", ut1::tui::ansiWhiteOnBlue);
    printLine(data[0], ut1::tui::ansiWhiteOnGray);
    // The plain-text table retains its rule; the TUI uses color to separate the header.
    for (size_t i = 2; i < data.size(); ++i)
        printLine(data[i], ut1::tui::ansiGreen);
    printLine(" q / Ctrl-C: quit", ut1::tui::ansiWhiteOnBlue);
    std::cout.flush();
}

/// Poll for exit keys while refreshing at the requested interval.
void runUi(unsigned intervalMs)
{
    ut1::tui::TerminalRawMode raw(STDIN_FILENO, true);
    ut1::tui::AlternateScreen screen;
    for (;;)
    {
        draw(readPower());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            const int key = ut1::tui::readKey(static_cast<int>(std::min<int64_t>(remaining, 250)));
            if (key == 'q' || key == 'Q' || key == 3) return;
        }
    }
}

}

/// Parse command-line options and run the one-shot or interactive display.
int main(int argc, char* argv[])
{
    try
    {
        const char* usage = "Show live power flow and battery telemetry on macOS.\n"
                            "\nUsage: $programName [OPTIONS]\n"
                            "\nExamples:\n"
                            "  $programName\n"
                            "  $programName --no-ui\n"
                            "  $programName --interval=2";
        ut1::CommandLineParser cl("tmacpower", usage,
            "\n$programName version $version ($compileDate) *** Copyright (c) 2026 Johannes Overmann *** https://github.com/jovermann/tmacpower",
            "0.1.0");
        cl.addHeader("\nOptions:\n");
        cl.addOption('n', "no-ui", "Print one plain-text sample and exit.");
        cl.addOption('i', "interval", "TUI refresh interval in seconds (0.2 to 60).", "SECONDS", "1");
        cl.parse(argc, argv);
        if (!cl.getArgs().empty()) cl.error("Unexpected positional arguments.");
        const double seconds = cl.getDouble("interval");
        if (!std::isfinite(seconds) || seconds < 0.2 || seconds > 60.0)
            cl.error("--interval must be between 0.2 and 60 seconds.");
        if (cl.getBool("no-ui"))
        {
            for (const auto& line : rows(readPower())) std::cout << line << '\n';
        }
        else
        {
            if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
                cl.error("The TUI needs a terminal; use --no-ui for plain text.");
            runUi(static_cast<unsigned>(seconds * 1000.0));
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "tmacpower: " << e.what() << '\n';
        return 1;
    }
}
