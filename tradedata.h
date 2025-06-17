#ifndef TRADEDATA_H
#define TRADEDATA_H

#include <QtCore/QDateTime>
#include <QtCore/QString>

// An enumeration to represent the side of a trade in a type-safe way
// This is better than using raw strings like "buy" or "sell"
enum class Side {
    Buy,
    Sell,
    Unknown
};

// A struct to represent a single market trade
// This is our C++ equivalent of a Pydantic model for trade data
struct Trade
{
    QDateTime timestamp;
    Side side;
    double price;
    double size;
};

#endif // TRADEDATA_H 