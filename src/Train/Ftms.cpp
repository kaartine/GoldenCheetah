#include "Ftms.h"

#include <QIODevice>
#include <QtMath>

#include <algorithm>
#include <cmath>

void ftms_parse_indoor_bike_data(QDataStream &ds, FtmsIndoorBikeData &bd)
{
    //quint16 flags, inst_speed, avg_speed, inst_cadence, avg_cadence, tot_energy, energy_per_hour, elapsed_time, remaining_time;
    //qint16 resistence_level, inst_power, avg_power;
    //quint8 energy_per_min, heart_rate, met_equivalent;
    quint16 dummy16;
    quint8 dummy8;

    ds >> bd.flags;

    if (!(bd.flags & FtmsIndoorBikeFlags::FTMS_MORE_DATA))
    {
        // If more data is not set, instant speed is present
        ds >> bd.inst_speed; // resolution: 0.01 km/h
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_AVERAGE_SPEED_PRESENT)
    {
        ds >> bd.avg_speed; // resolution: 0.01 km/h
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_INST_CADENCE_PRESENT)
    {
        ds >> bd.inst_cadence; // resolution: 0.5 rpm
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_AVERAGE_CADENCE_PRESENT)
    {
        ds >> bd.avg_cadence; // resolution: 0.5 rpm
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_TOTAL_DISTANCE_PRESENT)
    {
        ds >> dummy16 >> dummy8; // we don't care about this, so just read 24 bits
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_RESISTANCE_LEVEL_PRESENT)
    {
        ds >> bd.resistence_level; // resolution: unitless
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_INST_POWER_PRESENT)
    {
        ds >> bd.inst_power; // resolution: 1 watt
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_AVERAGE_POWER_PRESENT)
    {
        ds >> bd.avg_power; // resolution: 1 watt
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_EXPENDED_ENERGY_PRESENT)
    {
        ds >> bd.tot_energy >> bd.energy_per_hour >> bd.energy_per_min; // resolution: 1 kcal
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_HEART_RATE_PRESENT)
    {
        ds >> bd.heart_rate; // resolution: 1 bpm
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_METABOLIC_EQUIV_PRESENT)
    {
        ds >> bd.met_equivalent; // resolution: 1 MET
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_ELAPSED_TIME_PRESENT)
    {
        ds >> bd.elapsed_time; // resolution: 1 second
    }

    if (bd.flags & FtmsIndoorBikeFlags::FTMS_REMAINING_TIME_PRESENT)
    {
        ds >> bd.remaining_time; // resolution: 1 second
    }
}

bool ftms_parse_feature_data(const QByteArray &value, FtmsFeatureData &data)
{
    if (value.size() != 8) return false;

    QDataStream stream(value);
    stream.setByteOrder(QDataStream::LittleEndian);
    FtmsFeatureData parsed;
    stream >> parsed.machineFeatures >> parsed.targetSettings;
    if (stream.status() != QDataStream::Ok) return false;

    data = parsed;
    return true;
}

QByteArray ftms_control_point_command(const FtmsTargetCommand &target)
{
    if (!target.isValid()) return QByteArray();

    quint8 opcode = 0;
    switch (target.type) {
    case FtmsTargetType::Power:
        opcode = FtmsControlPointCommand::FTMS_SET_TARGET_POWER;
        break;
    case FtmsTargetType::Resistance:
        opcode = FtmsControlPointCommand::FTMS_SET_TARGET_RESISTANCE_LEVEL;
        break;
    case FtmsTargetType::None:
        return QByteArray();
    }

    QByteArray command;
    QDataStream stream(&command, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << opcode << target.value;
    return stream.status() == QDataStream::Ok ? command : QByteArray();
}

FtmsTargetCommand FtmsTargetController::requestPower(double watts)
{
    if (!std::isfinite(watts)) return FtmsTargetCommand();
    if (!powerRange.ready) {
        pendingTarget = FtmsTargetType::Power;
        pendingValue = watts;
        return FtmsTargetCommand();
    }

    clearPendingTarget();
    return targetCommand(FtmsTargetType::Power, watts, powerRange);
}

FtmsTargetCommand FtmsTargetController::requestResistance(double ratio)
{
    if (!std::isfinite(ratio)) return FtmsTargetCommand();
    if (!resistanceRange.ready) {
        pendingTarget = FtmsTargetType::Resistance;
        pendingValue = ratio;
        return FtmsTargetCommand();
    }

    clearPendingTarget();
    const double boundedRatio = std::clamp(ratio, 0.0, 1.0);
    const double width = static_cast<qint64>(resistanceRange.maximum) -
                         resistanceRange.minimum;
    return targetCommand(FtmsTargetType::Resistance,
                         resistanceRange.minimum + width * boundedRatio,
                         resistanceRange);
}

FtmsRangeResult FtmsTargetController::updatePowerRange(const QByteArray &value)
{
    return updateRange(value, powerRange, FtmsTargetType::Power);
}

FtmsRangeResult FtmsTargetController::updateResistanceRange(const QByteArray &value)
{
    return updateRange(value, resistanceRange, FtmsTargetType::Resistance);
}

void FtmsTargetController::clearPendingTarget()
{
    pendingTarget = FtmsTargetType::None;
    pendingValue = 0.0;
}

void FtmsTargetController::reset()
{
    powerRange = TargetRange();
    resistanceRange = TargetRange();
    clearPendingTarget();
}

FtmsRangeResult FtmsTargetController::updateRange(
        const QByteArray &value, TargetRange &range, FtmsTargetType type)
{
    FtmsRangeResult result;
    if (value.size() != 6) return result;

    QDataStream stream(value);
    stream.setByteOrder(QDataStream::LittleEndian);
    TargetRange parsed;
    stream >> parsed.minimum >> parsed.maximum >> parsed.increment;
    if (stream.status() != QDataStream::Ok ||
        parsed.minimum > parsed.maximum || parsed.increment == 0) {
        return result;
    }

    parsed.ready = true;
    range = parsed;
    result.accepted = true;
    result.minimum = range.minimum;
    result.maximum = range.maximum;
    result.increment = range.increment;
    result.command = pendingCommand(type, range);
    return result;
}

FtmsTargetCommand FtmsTargetController::targetCommand(
        FtmsTargetType type, double value, const TargetRange &range)
{
    if (!range.ready || !std::isfinite(value)) return FtmsTargetCommand();

    const double bounded = std::clamp(
            value, static_cast<double>(range.minimum),
            static_cast<double>(range.maximum));
    const double steps = (bounded - range.minimum) / range.increment;
    const qint64 roundedSteps = qRound64(steps);
    const qint64 scaled = static_cast<qint64>(range.minimum) +
                          roundedSteps * range.increment;
    const qint64 boundedScaled = std::clamp(
            scaled, static_cast<qint64>(range.minimum),
            static_cast<qint64>(range.maximum));
    return FtmsTargetCommand{type, static_cast<qint16>(boundedScaled)};
}

FtmsTargetCommand FtmsTargetController::pendingCommand(
        FtmsTargetType type, const TargetRange &range)
{
    if (pendingTarget != type) return FtmsTargetCommand();

    const double value = pendingValue;
    clearPendingTarget();
    if (type == FtmsTargetType::Resistance) {
        const double boundedRatio = std::clamp(value, 0.0, 1.0);
        const double width = static_cast<qint64>(range.maximum) - range.minimum;
        return targetCommand(type, range.minimum + width * boundedRatio, range);
    }

    return targetCommand(type, value, range);
}
