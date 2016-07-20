///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2016 DreamWorks Animation LLC. 
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// *       Redistributions of source code must retain the above
//         copyright notice, this list of conditions and the following
//         disclaimer.
// *       Redistributions in binary form must reproduce the above
//         copyright notice, this list of conditions and the following
//         disclaimer in the documentation and/or other materials
//         provided with the distribution.
// *       Neither the name of DreamWorks Animation nor the names of its
//         contributors may be used to endorse or promote products
//         derived from this software without specific prior written
//         permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////
///
/// @file DcxChannelSet.cpp

#include "DcxChannelAlias.h"
#include "DcxChannelContext.h"
#include "DcxChannelSet.h"
#include "DcxChannelDefs.h"

#include <map>
#include <algorithm> // for std::sort in some compilers
#include <string.h> // for strcmp in some compilers


OPENDCX_INTERNAL_NAMESPACE_HEADER_ENTER

//-----------------------------------------------------------------------------

// This should be in IlmImf somewhere...
inline
const char*
pixelTypeString (OPENEXR_IMF_NAMESPACE::PixelType type)
{
    switch (type)
    {
    case OPENEXR_IMF_NAMESPACE::HALF:  return "half";
    case OPENEXR_IMF_NAMESPACE::FLOAT: return "float";
    case OPENEXR_IMF_NAMESPACE::UINT:  return "uint";
    default: return "invalid";
    }
}

//
// Split a string up based on a delimiter list.
//

inline
void
split (const std::string& src,
       const char* delimiters,
       std::vector<std::string>& tokens)
{
    for (size_t index=0; ;) {
        size_t a = src.find_first_of(delimiters, index);
        if (a == std::string::npos) {
            if (src.length() > index)
                tokens.push_back(src.substr(index, std::string::npos));
            break;
        }
        if (a != index)
            tokens.push_back(src.substr(index, a-index));
        index = a+1;
    }
}


//
// Strip characters from the delimiter list out of the string.
//

inline
void
strip (std::string& src,
       const char* delimiters=" \n\t\r")
{
    std::string s0(src);
    std::string s1; s1.reserve(s0.length());
    std::string* p0 = &s0;
    std::string* p1 = &s1;
    for (const char* dp = delimiters; *dp; ++dp) {
        for (std::string::iterator i=p0->begin(); i != p0->end(); ++i)
            if (*i != *dp)
                p1->push_back(*i);
        p0->clear();
        std::string* t = p0; p0 = p1; p1 = t; // swap pointers
    }
    src = *p0;
}


//
// Split the name into layer & chan, if possible.
//

void
splitName (const char* name,
           std::string& layer,
           std::string& chan)
{
    std::string s(name);
    strip(s); // Remove any whitespace

    // Can we separate layer & chan strings?
    size_t a = s.find_last_of('.');
    if (a > 0 && a != std::string::npos)
    {
        layer = s.substr(0, a);
        chan  = s.substr(a+1);
        return;
    }
    // Only chan, no layer:
    chan = name;
    layer.clear();
}


//-----------------------------------------------------------------------------
//
// Standard predefined layer / channel combinations as recommended
// by OpenEXR documentation (with some extras....)
//
//-----------------------------------------------------------------------------

struct StandardChannel
{
    const char*     layer_name;             // User-facing layer name
    const char*     channel_name;           // User-facing channel name
    //
    const char*     match_list;             // List of strings to name-match to - keep upper-case!
    //
    const char*     dflt_io_name;           // Default file I/O channel name
    Imf::PixelType  dflt_io_pixel_type;     // Default file I/O data type
    //
    ChannelIdx      ordering_index;         // Index used to determine channel order (R before G, G before A)
};

//
// **************************************************************
// *                                                            *
// *        KEEP THIS TABLE IN SYNC WITH ChannelDefs.h!         *
// *         There should be one entry for each unique          *
// *            ChannelIdx 'Chan_*' definition.                 *
// *                                                            *
// *     See OpenEXR TechnicalIntroduction.pdf, pages 19-20     *
// *                                                            *
// **************************************************************
//
static StandardChannel g_standard_channel_table[] =
{
    {"invalid",  "invalid",  "",          "",             Imf::HALF,      Chan_Invalid }, // 0 (Chan_Invalid)
    //
    //<usr layer> <usr chan> <match list> <dflt I/O name> <dflt I/O type> <ordering index>
    //
    { "rgba",    "R",        "R,RED",     "R",            Imf::HALF,      Chan_R         }, // 1
    { "rgba",    "G",        "G,GREEN",   "G",            Imf::HALF,      Chan_G         }, // 2
    { "rgba",    "B",        "B,BLUE",    "B",            Imf::HALF,      Chan_B         }, // 3
    { "rgba",    "A",        "A,ALPHA",   "A",            Imf::HALF,      Chan_A         }, // 4
    //
    { "opacity", "R",        "AR,RA",     "AR",           Imf::HALF,      Chan_AR        }, // 5
    { "opacity", "G",        "AG,GA",     "AG",           Imf::HALF,      Chan_AG        }, // 6
    { "opacity", "B",        "AB,BA",     "AB",           Imf::HALF,      Chan_AB        }, // 7
    //
    { "yuv",     "Y",        "Y",         "Y",            Imf::HALF,      Chan_Y         }, // 8
    { "yuv",     "RY",       "RY",        "RY",           Imf::HALF,      Chan_RY        }, // 9
    { "yuv",     "BY",       "BY",        "BY",           Imf::HALF,      Chan_BY        }, // 10
    //
    { "depth",   "Z",        "Z",         "Z",            Imf::FLOAT,     Chan_Z         }, // 11
    { "depth",   "ZFront",   "ZF,ZFRONT", "",             Imf::FLOAT,     Chan_ZFront    }, // 12 - placeholder! (TODO: ditch 'ZFront' completely and just use 'Z'...?)
    { "depth",   "ZBack",    "ZB,ZBACK",  "ZBack",        Imf::FLOAT,     Chan_ZBack     }, // 13
    //
    // These are additional common channel types:
    //
    { "spmask",  "flags",    "FLAGS,3",   "spmask.flags", Imf::HALF,      Chan_DeepFlags }, // 14 - translate spmask.3 to spmask.flags for bkwd-compat
    { "spmask",  "1",        "1",         "spmask.1",     Imf::FLOAT,     Chan_SpBits1   }, // 15
    { "spmask",  "2",        "2",         "spmask.2",     Imf::FLOAT,     Chan_SpBits2   }, // 16
    //
    { "tex",     "s",        "S",         "tex.s",        Imf::HALF,      Chan_UvS       }, // 17 (TODO: is 'uv' a better layer name?)
    { "tex",     "t",        "T",         "tex.t",        Imf::HALF,      Chan_UvT       }, // 18
    { "tex",     "p",        "P",         "tex.p",        Imf::HALF,      Chan_UvP       }, // 19
    { "tex",     "q",        "Q",         "tex.q",        Imf::HALF,      Chan_UvQ       }, // 20
    //
    { "id",      "0",        "ID,ID0",    "ID",           Imf::UINT,      Chan_ID0       }, // 21
    { "id",      "1",        "ID1",       "ID1",          Imf::UINT,      Chan_ID1       }, // 22
    { "id",      "2",        "ID2",       "ID2",          Imf::UINT,      Chan_ID2       }, // 23
    { "id",      "3",        "ID3",       "ID3",          Imf::UINT,      Chan_ID3       }, // 24
    //
    { "cutout",  "A",        "",          "cutout.A",     Imf::HALF,      Chan_CutoutA   }, // 25
    { "cutout",  "AR",       "",          "cutout.AR",    Imf::HALF,      Chan_CutoutAR  }, // 26
    { "cutout",  "AG",       "",          "cutout.AG",    Imf::HALF,      Chan_CutoutAG  }, // 27
    { "cutout",  "AB",       "",          "cutout.AB",    Imf::HALF,      Chan_CutoutAB  }, // 28
    { "cutout",  "Z",        "",          "cutout.Z",     Imf::FLOAT,     Chan_CutoutZ   }, // 29
    //
    //
    { NULL,      NULL,       NULL,        NULL,           Imf::HALF,      Chan_Invalid } // EOT

};

//
// Static map of channel-matching strings to a StandardChannel
//
static std::map<std::string, StandardChannel*> g_standard_channel_matching_map;


//
// Initializes a map of standard-channel matching strings
//

struct BuildStandardChannels
{
    BuildStandardChannels ()
    {
        for (StandardChannel* c=g_standard_channel_table; c->layer_name; ++c)
        {
            if (!c->match_list || !c->match_list[0])
                continue;
            std::vector<std::string> tokens;
            split(std::string(c->match_list), ",", tokens);
            for (size_t i=0; i < tokens.size(); ++i)
            {
                strip(tokens[i]); // remove whitespace
                if (!tokens[i].empty())
                    g_standard_channel_matching_map[tokens[i]] = c;
            }
        }
    }
};
static BuildStandardChannels build_standard_channels;



//
// Returns the best matching pre-defined channel and layer for the channel
// name (with no layer prefix,).
// If there's a match it also sets the channel's default io name and pixeltype.
//

bool
matchStandardChannel (const char*     channel_name,
                      std::string&    std_layer_name,
                      std::string&    std_chan_name,
                      ChannelIdx&     std_channel,
                      std::string&    std_io_name,
                      Imf::PixelType& std_io_type)
{
    std_channel = Chan_Invalid;
    std_layer_name.clear();
    std_io_name.clear();

    if (!channel_name || !channel_name[0])
        return false;
    std::string name(channel_name);

    // First test for names that potentially collide like 'Y'&'y' or 'Z'&'z':
    /* Examples:
    if (name == "y")
    {
        // lower-case y takes precendence:
        std_channel    = Chan_VecY;
        std_layer_name = "vec";
        std_chan_name  = "y";
        std_io_name    = "y";
        std_io_type    = Imf::FLOAT;
        return true;
    }
    else if (name == "Y")
    {
        std_channel    = Chan_Y;
        std_layer_name = "yuv";
        std_chan_name  = "Y";
        std_io_name    = "Y";
        std_io_type    = Imf::HALF;
        return true;
    }
    */

    // No collision, see if the name's in the matching map:

    // To upper-case for matching map comparison:
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);

    std::map<std::string, StandardChannel*>::iterator it = g_standard_channel_matching_map.find(name);
    if (it == g_standard_channel_matching_map.end())
        return false;

    std_layer_name = it->second->layer_name;
    std_chan_name  = it->second->channel_name;
    std_channel    = it->second->ordering_index;
    std_io_name    = it->second->dflt_io_name;
    std_io_type    = it->second->dflt_io_pixel_type;

    return true;
}


//
// If the kind of channel is one of the predefined ones, return
// the common position that channel occupies in a layer.
// This allows channel names to 
// i.e.
//      if kind==Chan_R -> rgba position 0
//      if kind==Chan_A -> rgba position 3
//

int
getLayerPositionFromKind (ChannelIdx kind)
{
    if (kind <= Chan_Invalid || kind >= Chan_ArbitraryStart)
        return 0; // no idea

    // Determine layer position from kind.
    if      (kind <= Chan_A)          return kind - Chan_R;   // rgba
    else if (kind <= Chan_AB)         return kind - Chan_AR;  // opacity
    else if (kind <= Chan_BY)         return kind - Chan_RY;  // yuv
    else if (kind == Chan_Z || kind == Chan_ZFront) return 0; // depth
    else if (kind == Chan_ZBack)      return 1;               // depth
    else if (kind <= Chan_SpBitsLast) return kind - Chan_DeepFlags; // spmask
    else if (kind <= Chan_UvQ)        return kind - Chan_UvS; // tex
#if 0
    // TODO: how to handle more obscure layers?
    else if (kind <= Chan_ID3)        return kind - Chan_ID0; // id
    else if (kind <= Chan_CutoutZ)    return kind - Chan_CutoutA; // cutout
#endif

    return 0;
}


//-----------------------------------------------------------------------------
//
//    class ChannelSet
//
//-----------------------------------------------------------------------------

//
// Iterator to this always returns 0 (Chan_Invalid):
//
/*static*/ ChannelIdxSet ChannelSet::m_npos;


/*friend*/
std::ostream&
operator << (std::ostream& os,
             const ChannelSet::iterator& it)
{
    return os << it.channel();
}

//
// Print channel or layer.channel name to output stream.
// If the ChannelContext is NULL only the ChannelIdx number will be
// printed, otherwise the channel names will.
//

void ChannelSet::print (const char* prefix,
                        std::ostream& os,
                        const ChannelContext* ctx) const
{
    if (prefix && prefix[0])
        os << prefix;
    os << "[";
    if (m_mask.size() == 0)
        os << "none";
    else if (m_mask.size() == 1)
        if (ctx)
            ctx->printChannelFullName(os, *m_mask.begin());
        else
            os << *m_mask.begin();
    else
    {
        int i = 0;
        for (ChannelIdxSet::const_iterator z=m_mask.begin(); z != m_mask.end(); ++z)
        {
            if (i++ > 0)
                os << ",";
            if (ctx)
                ctx->printChannelFullName(os, *z);
            else
                os << *z;
        }
    }
    os << "]";
}

//
// Outputs the ChannelIdx number of the channels to the stream
//

/*friend*/
std::ostream&
operator << (std::ostream& os,
             const ChannelSet& b)
{
    b.print(NULL/*prefix*/, os, NULL/*ChannelContext*/);
    return os;
}


//-----------------------------------------------------------------------------
//
//    class ChannelAlias
//
//-----------------------------------------------------------------------------

ChannelAlias::ChannelAlias (const char*    name,
                            const char*    layer,
                            ChannelIdx     channel,
                            uint32_t       position,
                            const char*    io_name,
                            Imf::PixelType io_type) :
    m_name(name),
    m_layer(layer),
    //
    m_channel(channel),
    m_position(position),
    //
    m_io_name(io_name),
    m_io_type(io_type)
    //m_io_part(io_part)
{
    //
}


/*virtual*/
ChannelAlias::~ChannelAlias()
{
    //
}


ChannelAlias& ChannelAlias::operator = (const ChannelAlias& b)
{
    m_name     = b.m_name;
    m_layer    = b.m_layer;
    m_channel  = b.m_channel;
    m_position = b.m_position;
    m_io_name  = b.m_io_name;
    m_io_type  = b.m_io_type;
    //m_io_part  = b.m_io_part;
    return *this;
}

//
// Returns the fully-formed name - '<layer>.<channel>'
//

std::string
ChannelAlias::fullName () const
{
    if (!m_layer.empty())
        return m_layer + "." + m_name;
    return m_name;
}


//
// Default name used in EXR file I/O
//   ex. 'R'  vs. 'rgba.R'
//
// If not one of the standard channels
// this will be the same as fullName()
//

std::string
ChannelAlias::fileIOName() const
{
    if (m_io_name.empty())
        return fullName();
    return m_io_name;
}


bool
ChannelAlias::operator == (const ChannelAlias& b) const
{
    return (m_channel == b.m_channel);
}

//
// Output the full name of the channel to the stream
//

/*friend*/
std::ostream&
operator << (std::ostream& os,
             const ChannelAlias& b)
{
    return os << b.fullName();
}



//-----------------------------------------------------------------------------
//
//    class ChannelContext
//
//-----------------------------------------------------------------------------


//
// Initializes to set of standard channels.
//

ChannelContext::ChannelContext(bool addStandardChans) :
    m_last_assigned(Chan_ArbitraryStart-1)
{
    if (addStandardChans)
        addStandardChannels();
}


/*virtual*/
ChannelContext::~ChannelContext()
{
    for (size_t i=0; i < m_channelalias_list.size(); i++)
        delete m_channelalias_list[i];
}


void
ChannelContext::addStandardChannels()
{
    for (StandardChannel* c=g_standard_channel_table; c->layer_name; ++c)
    {
        if (strcmp(c->layer_name, "invalid")==0)
            continue; // skip Chan_Invalid
        addChannelAlias(c->channel_name,
                        c->layer_name,
                        c->ordering_index,
                        getLayerPositionFromKind(c->ordering_index),
                        c->dflt_io_name,
                        c->dflt_io_pixel_type);
    }
}



//
// Get channel or <layer>.<channel> name from a ChannelIdx.
// Returns 'unknown' if it doesn't exist.
//

const char*
ChannelContext::getChannelName (ChannelIdx channel) const
{
    if (channel == Chan_Invalid)
        return "invalid";
    if (channel < m_last_assigned)
    {
        ChannelIdxToListMap::const_iterator it = m_channelalias_channel_map.find(channel);
        if (it != m_channelalias_channel_map.end())
            return m_channelalias_list[it->second]->name().c_str();
    }
    return "unknown";
}

std::string
ChannelContext::getChannelFullName (ChannelIdx channel) const
{
    if (channel == Chan_Invalid)
        return std::string("invalid");
    if (channel < m_last_assigned)
    {
        ChannelIdxToListMap::const_iterator it = m_channelalias_channel_map.find(channel);
        if (it != m_channelalias_channel_map.end())
            return m_channelalias_list[it->second]->fullName();
    }
    return std::string("unknown");
}



//
// Find a channel alias
//

ChannelAlias*
ChannelContext::findChannelAlias (const std::string& name) const
{
    if (!name.empty())
    {
        AliasNameToListMap::const_iterator it = m_channelalias_name_map.find(name);
        if (it != m_channelalias_name_map.end())
            return m_channelalias_list[it->second];
    }
    return NULL;
}

ChannelAlias*
ChannelContext::findChannelAlias (const char* name) const
{
    if (!name || !name[0])
        return NULL;
    return findChannelAlias(std::string(name));
}

ChannelAlias*
ChannelContext::findChannelAlias (ChannelIdx channel) const
{
    if (channel > Chan_Invalid)
    {
        ChannelIdxToListMap::const_iterator it = m_channelalias_channel_map.find(channel);
        if (it != m_channelalias_channel_map.end())
            return m_channelalias_list[it->second];
    }
    return NULL;
}


//
// Add a ChannelAlias to shared lists.
// Context takes ownership of pointer and deletes it in destructor.
//

ChannelAlias*
ChannelContext::addChannelAlias (ChannelAlias* alias)
{
    if (!alias)
        return NULL;
    for (size_t i=0; i < m_channelalias_list.size(); i++)
        if (m_channelalias_list[i] == alias)
            return alias; // ignore duplicates

    // No specfic channel slot requested, assign the next one in the list:
    if (alias->m_channel == Chan_Invalid)
        alias->m_channel = ++m_last_assigned;

    const size_t index = m_channelalias_list.size();
    m_channelalias_list.push_back(alias);

    if (findChannelAlias(alias->m_channel)==NULL)
        m_channelalias_channel_map[alias->m_channel] = index;

    // Add name keys for full name '<layer>.<channel>' and the file io name.
    // Don't overwrite existing assignments:
    if (findChannelAlias(alias->fullName())==NULL)
        m_channelalias_name_map[alias->fullName()] = index;
    if (!alias->m_io_name.empty() && findChannelAlias(alias->m_io_name)==NULL)
        m_channelalias_name_map[alias->m_io_name] = index;

    return alias;
}


//
// Create a new channel and add to shared lists
//

ChannelAlias*
ChannelContext::addChannelAlias (const std::string& chan_name,
                                 const std::string& layer_name,
                                 ChannelIdx         channel,
                                 uint32_t           position,
                                 const std::string& io_name,
                                 Imf::PixelType     io_type)
{
    return addChannelAlias(new ChannelAlias(chan_name.c_str(),
                                            layer_name.c_str(),
                                            channel,
                                            position,
                                            io_name.c_str(),
                                            io_type));
}


//
// Get or create a channel/alias & possibly a new layer.
// Return a ChannelAlias or NULL if not able to create it.
//
// TODO: This logic is a little confused atm - make sure there's a clear way to
// repeatedly map the same channel name to the same alias.
// For example, when a standard channel is matched we only create a single alias using the original
// chan name which may confuse things if the name gets remapped (spmask.3->spmask.flags)
//
// We should create two aliases, one with the provided name and one with the standard name.
//

ChannelAlias*
ChannelContext::getChannelAlias (const char* name)
{
    if (!name || !name[0])
        return NULL; // don't crash!

    // Does alias already exist?
    ChannelAlias* chan = findChannelAlias(name);
    if (chan)
        return chan;

    // Not found, see if name can be split into separate layer/chan strings:
    std::string layer_name, chan_name;
    splitName(name, layer_name, chan_name);

    ChannelIdx channel = Chan_Invalid;
    int position = 0;

    // Does channel string corresponds to any standard ones? If so we
    // can determine the 'kind' of channel from it:
    std::string    std_layer_name = "";
    std::string    std_chan_name  = "";
    std::string    std_io_name    = "";
    Imf::PixelType std_io_type    = Imf::HALF;
    if (matchStandardChannel(chan_name.c_str(),
                             std_layer_name,
                             std_chan_name,
                             channel,
                             std_io_name,
                             std_io_type))
    {
        // Channel name matches one of the standard ones, so
        // get it's layer position:
        position = getLayerPositionFromKind(channel);
        // Update layer name if it's empty:
        // TODO: don't think we need to do this - we should
        //      handle channel names without layer prefixes!
        if (layer_name.empty())
            layer_name = std_layer_name;
        // TODO: change chan_name to match the standard name?  No, we
        //    probably want to add two ChannelAlias, one with the
        //    provided name and one with the std name.
        //chan_name = std_chan_name;
    }
    else
    {
        // Channel string unrecognized, this is a custom channel
        // so default to 'other' if there's no layer in name:
        // TODO: don't think we need to do this - we should
        //      handle channel names without layer prefixes!
        if (layer_name.empty())
            layer_name = "other";
    }

    // Does the full name now match any existing aliases?
    if (!layer_name.empty())
    {
        std::string full_name = layer_name + "." + chan_name;
        chan = findChannelAlias(full_name);
        if (chan)
            return chan;
    }

    // Create new alias, and possibly a new layer.
    // If channel is still Chan_Invalid it will get assigned to next
    // available slot when added to context:
    chan = addChannelAlias(chan_name,
                           layer_name,
                           channel,
                           position,
                           std_io_name,
                           std_io_type);
    if (!chan)
        return NULL; // shouldn't happen...

    ChanOrder chan_order;
    chan_order.channel = chan->channel();
    chan_order.order   = chan->layerPosition();

    LayerNameToListMap::iterator it = m_layer_name_map.find(layer_name);
    if (it == m_layer_name_map.end())
    {
        m_layers.push_back(Layer());
        Layer& new_layer = m_layers[m_layers.size()-1];
        new_layer.name = layer_name;
        new_layer.channels.push_back(chan_order);
        m_layer_name_map[layer_name] = m_layers.size()-1;
    }
    else
    {
        m_layers[it->second].channels.push_back(chan_order);
    }

    return chan;
}


OPENDCX_INTERNAL_NAMESPACE_HEADER_EXIT
