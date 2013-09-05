
#include "parser.hpp"

#include <cctype>
#include <stdexcept>
#include <sstream>

#include <components/misc/stringops.hpp>

#include "../world/columns.hpp"
#include "../world/data.hpp"
#include "../world/idcollection.hpp"

#include "booleannode.hpp"
#include "ornode.hpp"
#include "andnode.hpp"
#include "notnode.hpp"
#include "textnode.hpp"
#include "valuenode.hpp"

namespace CSMFilter
{
    struct Token
    {
        enum Type
        {
            Type_EOS,
            Type_None,
            Type_String,
            Type_Number,
            Type_Open,
            Type_Close,
            Type_OpenSquare,
            Type_CloseSquare,
            Type_Comma,
            Type_OneShot,
            Type_Keyword_True, ///< \attention Keyword enums must be arranged continuously.
            Type_Keyword_False,
            Type_Keyword_And,
            Type_Keyword_Or,
            Type_Keyword_Not,
            Type_Keyword_Text,
            Type_Keyword_Value
        };

        Type mType;
        std::string mString;
        double mNumber;

        Token (Type type = Type_None);

        Token (const std::string& string);

        Token (double number);

        operator bool() const;
    };

    Token::Token (Type type) : mType (type) {}

    Token::Token (const std::string& string) : mType (Type_String), mString (string) {}

    Token::Token (double number) : mType (Type_Number), mNumber (number) {}

    Token::operator bool() const
    {
        return mType!=Type_None;
    }

    bool operator== (const Token& left, const Token& right)
    {
        if (left.mType!=right.mType)
            return false;

        switch (left.mType)
        {
            case Token::Type_String: return left.mString==right.mString;
            case Token::Type_Number: return left.mNumber==right.mNumber;

            default: return true;
        }
    }
}

CSMFilter::Token CSMFilter::Parser::getStringToken()
{
    std::string string;

    int size = static_cast<int> (mInput.size());

    for (; mIndex<size; ++mIndex)
    {
        char c = mInput[mIndex];

        if (std::isalpha (c) || c==':' || c=='_' || (!string.empty() && std::isdigit (c)) || c=='"' ||
            (!string.empty() && string[0]=='"'))
            string += c;
        else
            break;

        if (c=='"' && string.size()>1)
        {
            ++mIndex;
            break;
        }
    };

    if (!string.empty())
    {
        if (string[0]=='"' && (string[string.size()-1]!='"' || string.size()<2) )
        {
            error();
            return Token (Token::Type_None);
        }

        if (string[0]!='"' && string[string.size()-1]=='"')
        {
            error();
            return Token (Token::Type_None);
        }

        if (string[0]=='"')
            return string.substr (1, string.size()-2);
    }

    return checkKeywords (string);
}

CSMFilter::Token CSMFilter::Parser::getNumberToken()
{
    std::string string;

    int size = static_cast<int> (mInput.size());

    bool hasDecimalPoint = false;
    bool hasDigit = false;

    for (; mIndex<size; ++mIndex)
    {
        char c = mInput[mIndex];

        if (std::isdigit (c))
        {
            string += c;
            hasDigit = true;
        }
        else if (c=='.' && !hasDecimalPoint)
        {
            string += c;
            hasDecimalPoint = true;
        }
        else if (string.empty() && c=='-')
            string += c;
        else
            break;
    }

    if (!hasDigit)
    {
        error();
        return Token (Token::Type_None);
    }

    float value;
    std::istringstream stream (string.c_str());
    stream >> value;

    return value;
}

CSMFilter::Token CSMFilter::Parser::checkKeywords (const Token& token)
{
    static const char *sKeywords[] =
    {
        "true", "false",
        "and", "or", "not",
        "string", "value",
        0
    };

    std::string string = Misc::StringUtils::lowerCase (token.mString);

    for (int i=0; sKeywords[i]; ++i)
        if (sKeywords[i]==string || (string.size()==1 && sKeywords[i][0]==string[0]))
            return Token (static_cast<Token::Type> (i+Token::Type_Keyword_True));

    return token;
}

CSMFilter::Token CSMFilter::Parser::getNextToken()
{
    int size = static_cast<int> (mInput.size());

    char c = 0;

    for (; mIndex<size; ++mIndex)
    {
        c = mInput[mIndex];

        if (c!=' ')
            break;
    }

    if (mIndex>=size)
        return Token (Token::Type_EOS);

    switch (c)
    {
        case '(': ++mIndex; return Token (Token::Type_Open);
        case ')': ++mIndex; return Token (Token::Type_Close);
        case '[': ++mIndex; return Token (Token::Type_OpenSquare);
        case ']': ++mIndex; return Token (Token::Type_CloseSquare);
        case ',': ++mIndex; return Token (Token::Type_Comma);
        case '!': ++mIndex; return Token (Token::Type_OneShot);
    }

    if (c=='"' || c=='_' || std::isalpha (c) || c==':')
        return getStringToken();

    if (c=='-' || c=='.' || std::isdigit (c))
        return getNumberToken();

    error();
    return Token (Token::Type_None);
}

boost::shared_ptr<CSMFilter::Node> CSMFilter::Parser::parseImp (bool allowEmpty, bool ignoreOneShot)
{
    if (Token token = getNextToken())
    {
        if (token==Token (Token::Type_OneShot))
            token = getNextToken();

        if (token)
            switch (token.mType)
            {
                case Token::Type_Keyword_True:

                    return boost::shared_ptr<CSMFilter::Node> (new BooleanNode (true));

                case Token::Type_Keyword_False:

                    return boost::shared_ptr<CSMFilter::Node> (new BooleanNode (false));

                case Token::Type_Keyword_And:
                case Token::Type_Keyword_Or:

                    return parseNAry (token);

                case Token::Type_Keyword_Not:
                {
                    boost::shared_ptr<CSMFilter::Node> node = parseImp();

                    if (mError)
                        return boost::shared_ptr<Node>();

                    return boost::shared_ptr<CSMFilter::Node> (new NotNode (node));
                }

                case Token::Type_Keyword_Text:

                    return parseText();

                case Token::Type_Keyword_Value:

                    return parseValue();

                case Token::Type_EOS:

                    if (!allowEmpty)
                        error();

                    return boost::shared_ptr<Node>();

                default:

                    error();
            }
    }

    return boost::shared_ptr<Node>();
}

boost::shared_ptr<CSMFilter::Node> CSMFilter::Parser::parseNAry (const Token& keyword)
{
    std::vector<boost::shared_ptr<Node> > nodes;

    Token token = getNextToken();

    if (token.mType!=Token::Type_Open)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    for (;;)
    {
        boost::shared_ptr<Node> node = parseImp();

        if (mError)
            return boost::shared_ptr<Node>();

        nodes.push_back (node);

        Token token = getNextToken();

        if (!token || (token.mType!=Token::Type_Close && token.mType!=Token::Type_Comma))
        {
            error();
            return boost::shared_ptr<Node>();
        }

        if (token.mType==Token::Type_Close)
            break;
    }

    if (nodes.empty())
    {
        error();
        return boost::shared_ptr<Node>();
    }

    switch (keyword.mType)
    {
        case Token::Type_Keyword_And: return boost::shared_ptr<CSMFilter::Node> (new AndNode (nodes));
        case Token::Type_Keyword_Or: return boost::shared_ptr<CSMFilter::Node> (new OrNode (nodes));
        default: error(); return boost::shared_ptr<Node>();
    }
}

boost::shared_ptr<CSMFilter::Node> CSMFilter::Parser::parseText()
{
    Token token = getNextToken();

    if (token.mType!=Token::Type_Open)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    token = getNextToken();

    if (!token)
        return boost::shared_ptr<Node>();

    // parse column ID
    int columnId = -1;

    if (token.mType==Token::Type_Number)
    {
        if (static_cast<int> (token.mNumber)==token.mNumber)
            columnId = static_cast<int> (token.mNumber);
    }
    else if (token.mType==Token::Type_String)
    {
        columnId = CSMWorld::Columns::getId (token.mString);
    }

    if (columnId<0)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    token = getNextToken();

    if (token.mType!=Token::Type_Comma)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    // parse text pattern
    token = getNextToken();

    if (token.mType!=Token::Type_String)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    std::string text = token.mString;

    token = getNextToken();

    if (token.mType!=Token::Type_Close)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    return boost::shared_ptr<Node> (new TextNode (columnId, text));
}

boost::shared_ptr<CSMFilter::Node> CSMFilter::Parser::parseValue()
{
    Token token = getNextToken();

    if (token.mType!=Token::Type_Open)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    token = getNextToken();

    if (!token)
        return boost::shared_ptr<Node>();

    // parse column ID
    int columnId = -1;

    if (token.mType==Token::Type_Number)
    {
        if (static_cast<int> (token.mNumber)==token.mNumber)
            columnId = static_cast<int> (token.mNumber);
    }
    else if (token.mType==Token::Type_String)
    {
        columnId = CSMWorld::Columns::getId (token.mString);
    }

    if (columnId<0)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    token = getNextToken();

    if (token.mType!=Token::Type_Comma)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    // parse value
    double lower = 0;
    double upper = 0;
    ValueNode::Type lowerType = ValueNode::Type_Open;
    ValueNode::Type upperType = ValueNode::Type_Open;

    token = getNextToken();

    if (token.mType==Token::Type_Number)
    {
        // single value
        lower = upper = token.mNumber;
        lowerType = upperType = ValueNode::Type_Closed;
    }
    else
    {
        // interval
        if (token.mType==Token::Type_OpenSquare)
            lowerType = ValueNode::Type_Closed;
        else if (token.mType!=Token::Type_CloseSquare && token.mType!=Token::Type_Open)
        {
            error();
            return boost::shared_ptr<Node>();
        }

        token = getNextToken();

        if (token.mType!=Token::Type_Number)
        {
            error();
            return boost::shared_ptr<Node>();
        }

        lower = token.mNumber;

        token = getNextToken();

        if (token.mType!=Token::Type_Comma)
        {
            error();
            return boost::shared_ptr<Node>();
        }

        token = getNextToken();

        if (token.mType!=Token::Type_Number)
        {
            error();
            return boost::shared_ptr<Node>();
        }

        upper = token.mNumber;

        token = getNextToken();

        if (token.mType==Token::Type_CloseSquare)
            upperType = ValueNode::Type_Closed;
        else if (token.mType!=Token::Type_OpenSquare && token.mType!=Token::Type_Close)
        {
            error();
            return boost::shared_ptr<Node>();
        }
    }

    token = getNextToken();

    if (token.mType!=Token::Type_Close)
    {
        error();
        return boost::shared_ptr<Node>();
    }

    return boost::shared_ptr<Node> (new ValueNode (columnId, lowerType, upperType, lower, upper));
}

void CSMFilter::Parser::error()
{
    mError = true;
}

CSMFilter::Parser::Parser (const CSMWorld::Data& data)
: mIndex (0), mError (false), mData (data) {}

bool CSMFilter::Parser::parse (const std::string& filter, bool allowPredefined)
{
    // reset
    mFilter.reset();
    mError = false;
    mInput = filter;
    mIndex = 0;

    Token token;

    if (allowPredefined)
        token = getNextToken();

    if (!allowPredefined || token==Token (Token::Type_OneShot))
    {
        boost::shared_ptr<Node> node = parseImp (true, token!=Token (Token::Type_OneShot));

        if (mError)
            return false;

        if (getNextToken()!=Token (Token::Type_EOS))
        {
            error();
            return false;
        }

        if (node)
            mFilter = node;
        else
        {
            // Empty filter string equals to filter "true".
            mFilter.reset (new BooleanNode (true));
        }

        return true;
    }
    else if (token.mType==Token::Type_String && allowPredefined)
    {
        if (getNextToken()!=Token (Token::Type_EOS))
        {
            error();
            return false;
        }

        int index = mData.getFilters().searchId (token.mString);

        if (index==-1)
        {
            error();
            return false;
        }

        const CSMWorld::Record<CSMFilter::Filter>& record = mData.getFilters().getRecord (index);

        if (record.isDeleted())
        {
            error();
            return false;
        }

        return parse (record.get().mFilter, false);
    }
    else
    {
        error();
        return false;
    }
}

boost::shared_ptr<CSMFilter::Node> CSMFilter::Parser::getFilter() const
{
    if (mError)
        throw std::logic_error ("No filter available");

    return mFilter;
}