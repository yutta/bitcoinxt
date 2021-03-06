#include "blockheaderprocessor.h"
#include "blockannounce.h"
#include "chain.h"
#include "consensus/validation.h"
#include "main.h" // Misbehaving, UpdateBlockAvailability
#include "net.h"
#include "netmessagemaker.h"
#include "nodestate.h"
#include "primitives/block.h"
#include "util.h"
#include "inflightindex.h"
#include "utilprocessmsg.h" // BlockInFlightMarker
#include <boost/range/adaptor/reversed.hpp>
#include <deque>

using namespace std;

/** Maximum number of unconnecting headers announcements before DoS score */
const int MAX_UNCONNECTING_HEADERS = 10;

// Check if header connects with active chain
bool headerConnects(const CBlockHeader& h) {
    return mapBlockIndex.find(h.hashPrevBlock) != mapBlockIndex.end();
}

DefaultHeaderProcessor::DefaultHeaderProcessor(CConnman& connman, CNode* pfrom,
        InFlightIndex& i,
        ThinBlockManager& mg,
        BlockInFlightMarker& inFlight,
        std::function<void()> checkBlockIndex) :
    connman(connman), pfrom(pfrom), blocksInFlight(i), thinmg(mg), markAsInFlight(inFlight),
    checkBlockIndex(checkBlockIndex)
{
}

// maybeAnnouncement: Header *might* have been received as a block announcement.
CBlockIndex* DefaultHeaderProcessor::operator()(const std::vector<CBlockHeader>& headers,
        bool peerSentMax,
        bool maybeAnnouncement)
{
    CBlockIndex* pindexLast = acceptHeaders(headers);

    NodeStatePtr(pfrom->id)->unconnectingHeaders = 0;

    if (pindexLast)
        UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

    if (peerSentMax && pindexLast) {
        // Headers message had its maximum size; the peer may have more headers.
        // TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
        // from there instead.
        LogPrint(Log::NET, "more getheaders (%d) to end to peer=%d (startheight:%d)\n", pindexLast->nHeight, pfrom->id, pfrom->nStartingHeight);
        connman.PushMessage(pfrom, NetMsg(pfrom, NetMsgType::GETHEADERS,
                                          chainActive.GetLocator(pindexLast), uint256()));
    }

    if (pindexLast && maybeAnnouncement && hasEqualOrMoreWork(pindexLast)) {

        std::vector<CBlockIndex*> toFetch = findMissingBlocks(pindexLast);

        // We may or may not start downloading the blocks
        // from this peer now.
        suggestDownload(toFetch, pindexLast);
    }

    checkBlockIndex();
    return pindexLast;
}

CBlockIndex* DefaultHeaderProcessor::acceptHeaders(
        const std::vector<CBlockHeader>& headers) {

    CBlockIndex *pindexLast = nullptr;
    for (const CBlockHeader& header : headers) {
        CValidationState state;
        if (pindexLast != nullptr && header.hashPrevBlock != pindexLast->GetBlockHash()) {
            Misbehaving(pfrom->GetId(), 20, "non-continuous header sequence");
            throw BlockHeaderError("non-continuous headers sequence");
        }
        if (!AcceptBlockHeader(header, state, &pindexLast)) {
            int nDoS;
            if (state.IsInvalid(nDoS)) {
                if (nDoS > 0)
                    Misbehaving(pfrom->GetId(), nDoS, "invalid header");
                throw BlockHeaderError("invalid header received");
            }
        }
    }
    return pindexLast;
}

std::vector<CBlockIndex*> DefaultHeaderProcessor::findMissingBlocks(CBlockIndex* last) {
    assert(last);

    std::deque<CBlockIndex*> toFetch;
    CBlockIndex* walk = last;

    const int WALK_LIMIT = 144; // one day
    int walked = 0;

    // Calculate all the blocks we'd need to switch to last, up to a limit.
    do {

        if (++walked > WALK_LIMIT) {
            // We're far behind. No gain in direct fetch.
            return std::vector<CBlockIndex*>();
        }

        if (chainActive.Contains(walk))
            break;

        if (walk->nStatus & BLOCK_HAVE_DATA)
            continue;

        if (blocksInFlight.isInFlight(walk->GetBlockHash()))
            continue;

        // We don't have this block, and it's not yet in flight.
        toFetch.push_back(walk);

        // Avoid out of order fetching, trim off the newest block. Out of order
        // fetching is conceptually fine, but confuses rpc tests that use comptool.
        if (toFetch.size() > MAX_BLOCKS_IN_TRANSIT_PER_PEER)
            toFetch.pop_front();

    } while ((walk = walk->pprev));

    return std::vector<CBlockIndex*>(begin(toFetch), end(toFetch));
}

bool DefaultHeaderProcessor::hasEqualOrMoreWork(CBlockIndex* last) {
    return last->IsValid(BLOCK_VALID_TREE)
        && chainActive.Tip()->nChainWork <= last->nChainWork;
}

void DefaultHeaderProcessor::suggestDownload(const std::vector<CBlockIndex*>& toFetch, CBlockIndex* last) {
    std::vector<CInv> toGet;

    for (auto b : boost::adaptors::reverse(toFetch)) {

        BlockAnnounceReceiver ann(b->GetBlockHash(),
                                  connman, *pfrom, thinmg, blocksInFlight);

        // Stop if we don't want to download this block now.
        // Won't want next.
        if (!ann.onBlockAnnounced(toGet))
            break;

        // This block has been requested from peer.
        markAsInFlight(pfrom->id, b->GetBlockHash(), Params().GetConsensus(), nullptr);
    }

    if (!toGet.empty()) {
        LogPrint(Log::NET, "Downloading blocks toward %s (%d) via headers direct fetch\n",
                last->GetBlockHash().ToString(), last->nHeight);
        connman.PushMessage(pfrom, NetMsg(pfrom, NetMsgType::GETDATA, toGet));
    }
}

// If we have a header from a peer that does not connect
// to our active chain, try to retrieve any missing to connect it.
//
// Return: If header request was needed. In this case,
// the current header cannot be processed.
bool DefaultHeaderProcessor::requestConnectHeaders(const CBlockHeader& h,
                                                   CConnman& connman, CNode& from,
                                                   bool bumpUnconnecting)
{
    if (headerConnects(h))
        return false;

    UpdateBlockAvailability(from.id, h.GetHash());

    LogPrint(Log::NET, "Headers for %s do not connect. We don't have pprev %s peer=%d\n",
            h.GetHash().ToString(), h.hashPrevBlock.ToString(), from.id);


    connman.PushMessage(&from, NetMsg(&from, NetMsgType::GETHEADERS,
                                      chainActive.GetLocator(pindexBestHeader),
                                      uint256()));

    if (!bumpUnconnecting)
        return true;

    int unconnecting = ++NodeStatePtr(from.id)->unconnectingHeaders;
    if (unconnecting % MAX_UNCONNECTING_HEADERS == 0)
        Misbehaving(from.id, 20, "unconnecting-headers");

    return true;
}
