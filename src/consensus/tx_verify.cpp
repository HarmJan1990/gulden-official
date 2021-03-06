// Copyright (c) 2017-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// File contains modifications by: The Gulden developers
// All modifications:
// Copyright (c) 2017-2018 The Gulden developers
// Authored by: Malcolm MacLeod (mmacleod@webmail.co.za)
// Distributed under the GULDEN software license, see the accompanying
// file COPYING

#include "tx_verify.h"

#include "consensus.h"
#include "script/interpreter.h"
#include "validation.h"

// TODO remove the following dependencies
#include "chain.h"
#include "coins.h"
#include "utilmoneystr.h"

//Gulden dependencies
#include "Gulden/util.h"

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    if (IsOldTransactionVersion(tx.nVersion))
    {
        for (const auto& txin : tx.vin) {
            if (!(txin.GetSequence(tx.nVersion) == CTxIn::SEQUENCE_FINAL))
                return false;
        }
    }
    else
    {
        for (const auto& txin : tx.vin) {
            if ( txin.FlagIsSet(CTxInFlags::OptInRBF) || txin.FlagIsSet(CTxInFlags::HasLock) )
                return false;
        }
    }
    return true;
}

std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx, int flags, std::vector<int>* prevHeights, const CBlockIndex& block)
{
    assert(prevHeights->size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2
                      && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn& txin = tx.vin[txinIndex];

        // Gulden - segsig - if we aren't using relative locktime then we don't have a sequence number at all.
        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if ((IsOldTransactionVersion(tx.nVersion) && (txin.GetSequence(tx.nVersion) & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG))
            || (!IsOldTransactionVersion(tx.nVersion) && (!txin.FlagIsSet(CTxInFlags::HasRelativeLock)))) {
            // The height of this input is not relevant for sequence locks
            (*prevHeights)[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = (*prevHeights)[txinIndex];

        if ((IsOldTransactionVersion(tx.nVersion) && (txin.GetSequence(tx.nVersion) & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG))
            || (!IsOldTransactionVersion(tx.nVersion) && (txin.FlagIsSet(HasTimeBasedRelativeLock))))
        {
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight-1, 0))->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            if (IsOldTransactionVersion(tx.nVersion))
                nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.GetSequence(tx.nVersion) & CTxIn::SEQUENCE_LOCKTIME_MASK) << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
            else
                nMinTime = std::max(nMinTime, nCoinTime + (txin.GetSequence(tx.nVersion) << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
        }
        else
        {
            if (IsOldTransactionVersion(tx.nVersion))
                nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.GetSequence(tx.nVersion) & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
            else
                nMinHeight = std::max(nMinHeight, nCoinHeight + (int)txin.GetSequence(tx.nVersion) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

bool EvaluateSequenceLocks(const CBlockIndex& block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int>* prevHeights, const CBlockIndex& block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    for (const auto& txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto& txout : tx.vout)
    {
        switch (txout.GetType())
        {
            case CTxOutType::ScriptLegacyOutput: nSigOps += txout.output.scriptPubKey.GetSigOpCount(false); break;
            case CTxOutType::StandardKeyHashOutput: nSigOps += 1; break;
            case CTxOutType::PoW2WitnessOutput: nSigOps += 1; break;
        }
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const CTxOut &prevout = inputs.AccessCoin(tx.vin[i].prevout).out;
        if (prevout.GetType() <= CTxOutType::ScriptLegacyOutput && prevout.output.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.output.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int64_t GetTransactionSigOpCost(const CTransaction& tx, const CCoinsViewCache& inputs, int flags)
{
    int64_t nSigOps = GetLegacySigOpCount(tx);

    if (tx.IsCoinBase())
        return nSigOps;

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs);
    }

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        //fixme: (GULDEN) (2.0) - Is this right? - make sure we are counting sigops in segsig scripts correctly
        const CTxOut &prevout = inputs.AccessCoin(tx.vin[i].prevout).out;
        switch (prevout.GetType())
        {
            case CTxOutType::StandardKeyHashOutput: nSigOps += 1; break;
            case CTxOutType::PoW2WitnessOutput: nSigOps += 1; break;
            case CTxOutType::ScriptLegacyOutput: /*already handled by GetP2SHSigOpCount*/ break;
        }
    }
    return nSigOps;
}

bool CheckTransaction(const CTransaction& tx, CValidationState &state, bool fCheckDuplicateInputs)
{
    // Basic checks that don't depend on any context
    if (tx.vin.empty())
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vin-empty");
    if (tx.vout.empty())
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vout-empty");
    // Size limits (this doesn't take the witness into account, as that hasn't been checked for malleability)
    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_SEGREGATED_SIGNATURES) > MAX_BLOCK_BASE_SIZE)
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-oversize");

    // Check for negative or overflow output values
    CAmount nValueOut = 0;
    for (const auto& txout : tx.vout)
    {
        if (txout.nValue < 0)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-vout-negative");
        if (txout.nValue > MAX_MONEY)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-vout-toolarge");
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-txouttotal-toolarge");
    }

    // Check for duplicate inputs - note that this check is slow so we skip it in CheckBlock
    if (fCheckDuplicateInputs) {
        std::set<COutPoint> vInOutPoints;
        for (const auto& txin : tx.vin)
        {
            if (!vInOutPoints.insert(txin.prevout).second)
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputs-duplicate");
        }
    }

    if (tx.IsCoinBase())
    {
        if (IsOldTransactionVersion(tx.nVersion))
        {
            if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100)
                return state.DoS(100, false, REJECT_INVALID, "bad-cb-length");
        }
        else
        {
            if (tx.vin[0].scriptSig.size() != 0)
                return state.DoS(100, false, REJECT_INVALID, "bad-cb-length");
            //fixme: (2.0) (HIGH) implement - check the segregatedSignatureData here?
        }
    }
    else
    {
        for (const auto& txin : tx.vin)
            if (txin.prevout.IsNull())
                return state.DoS(10, false, REJECT_INVALID, "bad-txns-prevout-null");
    }

    return true;
}


bool CheckTransactionContextual(const CTransaction& tx, CValidationState &state, int checkHeight, std::vector<CWitnessTxBundle>* pWitnessBundles)
{
    for (const CTxOut& txout : tx.vout)
    {
        if (IsPow2WitnessOutput(txout))
        {
            if ( txout.nValue < (nMinimumWitnessAmount * COIN) )
                return state.DoS(10, false, REJECT_INVALID, strprintf("PoW2 witness output smaller than %d NLG not allowed.", nMinimumWitnessAmount));

            CTxOutPoW2Witness witnessDetails; GetPow2WitnessOutput(txout, witnessDetails);
            if (witnessDetails.lockFromBlock == 0)
            {
                if (witnessDetails.lockUntilBlock - checkHeight < (576 * 30))
                    return state.DoS(10, false, REJECT_INVALID, "PoW2 witness locked for less than minimum of 1 month.");
            }
            else
            {
                if (witnessDetails.lockUntilBlock - witnessDetails.lockFromBlock < (576 * 30))
                    return state.DoS(10, false, REJECT_INVALID, "PoW2 witness locked for less than minimum of 1 month.");
            }
            if (witnessDetails.lockFromBlock == 0)
            {
                if (witnessDetails.lockUntilBlock - checkHeight > (3 * 365 * 576))
                    return state.DoS(10, false, REJECT_INVALID, "PoW2 witness locked for greater than maximum of 3 years.");
            }
            else
            {
                if (witnessDetails.lockUntilBlock - witnessDetails.lockFromBlock > (3 * 365 * 576))
                    return state.DoS(10, false, REJECT_INVALID, "PoW2 witness locked for greater than maximum of 3 years.");
            }

            uint64_t nUnused1, nUnused2;
            int64_t nWeight = GetPoW2RawWeightForAmount(txout.nValue, GetPoW2LockLengthInBlocksFromOutput(txout, checkHeight, nUnused1, nUnused2));
            if (nWeight < nMinimumWitnessWeight)
            {
                return state.DoS(10, false, REJECT_INVALID, "PoW2 witness has insufficient weight.");
            }

            if (pWitnessBundles)
            {
                if (witnessDetails.lockFromBlock == 0)
                {
                    pWitnessBundles->push_back(CWitnessTxBundle(CWitnessTxBundle::WitnessTxType::CreationType, std::make_pair(txout,std::move(witnessDetails))));
                }
                else
                {
                    if (tx.IsPoW2WitnessCoinBase())
                    {
                        pWitnessBundles->push_back(CWitnessTxBundle(CWitnessTxBundle::WitnessTxType::WitnessType, std::make_pair(txout, std::move(witnessDetails))));
                    }
                    else
                    {
                        bool matchedExistingBundle = false;
                        for (auto& bundle: *pWitnessBundles)
                        {
                            if ( (bundle.bundleType == CWitnessTxBundle::WitnessTxType::MergeType || bundle.bundleType == CWitnessTxBundle::WitnessTxType::SplitType) && (bundle.outputs[0].second.witnessKeyID == witnessDetails.witnessKeyID) && bundle.outputs[0].second.spendingKeyID == witnessDetails.spendingKeyID )
                            {
                                bundle.bundleType = CWitnessTxBundle::WitnessTxType::SplitType;
                                bundle.outputs.push_back(std::make_pair(txout, std::move(witnessDetails)));
                                matchedExistingBundle = true;
                                break;
                            }
                        }
                        if (!matchedExistingBundle)
                        {
                            CWitnessTxBundle spendBundle = CWitnessTxBundle(CWitnessTxBundle::WitnessTxType::SpendType, std::make_pair(txout, std::move(witnessDetails)));
                            pWitnessBundles->push_back(spendBundle);
                        }
                    }
                }
            }
        }
    }
    return true;
}

inline bool IsLockFromConsistent(const CTxOutPoW2Witness& inputDetails, const CTxOutPoW2Witness& outputDetails, uint64_t nInputHeight)
{
    // If lockfrom was previously 0 it must become the current block height instead.
    if (inputDetails.lockFromBlock == 0)
    {
        if (outputDetails.lockFromBlock != nInputHeight)
            return false;
    }
    else if (inputDetails.lockFromBlock != outputDetails.lockFromBlock)
        return false;
    return true;
}

CAmount CalculateWitnessPenaltyFee(const CTxOut& output)
{
    CTxOutPoW2Witness witnessDestination;
    if (!GetPow2WitnessOutput(output, witnessDestination))
    {
        return 0;
    }
    CAmount nPenalty = ((2*COIN)/100) * witnessDestination.failCount;
    //fixme: (2.0) define this as a constant somewhere (no magic numbers)
    if (nPenalty > COIN * 20)
        return COIN * 20;
    return nPenalty;
}

void IncrementWitnessFailCount(uint64_t& failCount)
{
    // Increment fail count (Increments in a sequence: 1 2 4 7 11 17 26 40 61 92 139 209 314 472 709 1064 1597 2396 3595 5393 8090 ...)
    failCount += (failCount/3);
    ++failCount;
    // Avoid overflow - by the time fail count is this high it no longer matters if we don't increment it further
    if (failCount > std::numeric_limits<uint64_t>::max() / 3)
        failCount = std::numeric_limits<uint64_t>::max() / 3;
}

inline bool HasSpendKey(const CTxIn& input, const CTxOutPoW2Witness& inputDetails)
{
    // 2 signatures, spending key and witness key.
    if (input.segregatedSignatureData.stack.size() != 2)
        return false;
    return true;
}

/*Witness transactions inputs/outputs fall into several different categories.
* 1) Creation of a new witnessing account: lockfrom must be 0.
* Output only.
*/

/*
* 2) Witnessing: amount should stay the same or increase, fail count can reduced by 1 or 0.
* If lockfrom was previously 0 it must be set to the height of the block that contains the input.
* Can only appear as a "witnesscoinbase" transaction.
* Input/Output.
* Signed by witness key.
*/
inline bool IsWitnessBundle(const CTxIn& input, const CTxOutPoW2Witness& inputDetails, const CTxOutPoW2Witness& outputDetails, CAmount nInputAmount, CAmount nOutputAmount, uint64_t nInputHeight)
{
    //fixme: (2.0) (HIGH) - test coinbase type.
    // Only 1 signature (witness key) - except in phase 3 embedded PoW coinbase where it is 0.
    if (input.segregatedSignatureData.stack.size() != 1 && input.segregatedSignatureData.stack.size() != 0)
        return false;
    // Amount in address should stay the same or increase
    if (nInputAmount > nOutputAmount)
        return false;
    // Keys and lock unchanged
    if (inputDetails.spendingKeyID != outputDetails.spendingKeyID)
        return false;
    if (inputDetails.witnessKeyID != outputDetails.witnessKeyID)
        return false;
    if (inputDetails.lockUntilBlock != outputDetails.lockUntilBlock)
        return false;
    if (!IsLockFromConsistent(inputDetails, outputDetails, nInputHeight))
        return false;
    // Fail must be reduced by 1 or 0 if it is already 0.
    if (inputDetails.failCount == outputDetails.failCount + 1)
        return true;
    if (outputDetails.failCount == 0)
        return true;
    return false;
}

/*
* 3) Spending, only amount should change, entire amount should be consumed, current height must be.
* Input only
* Signed by spending key.
*/
inline bool CWitnessTxBundle::IsValidSpendBundle(uint64_t nCheckHeight)
{
    if (outputs.size() != 0)
        return false;
    if (inputs.size() != 1)
        return false;
    if (inputs[0].second.lockUntilBlock >= nCheckHeight)
        return false;
    return true;
}

/*
* 4) Renewal, only failcount should change, must be multiplied by 2 then increased by 1.
* If lockfrom was previously 0 it must be set to the height of the block that contains the input.
* Input/Output.
* Signed by spending key.
*/
inline bool IsRenewalBundle(const CTxIn& input, const CTxOutPoW2Witness& inputDetails, const CTxOutPoW2Witness& outputDetails, CAmount nInputAmount, CAmount nOutputAmount, uint64_t nInputHeight)
{
    // Needs 2 signature (spending key)
    if (input.segregatedSignatureData.stack.size() != 2)
        return false;
    // Amount keys and lock unchanged.
    if (nInputAmount != nOutputAmount)
        return false;
    if (inputDetails.spendingKeyID != outputDetails.spendingKeyID)
        return false;
    if (inputDetails.witnessKeyID != outputDetails.witnessKeyID)
        return false;
    if (inputDetails.lockUntilBlock != outputDetails.lockUntilBlock)
        return false;
    if (!IsLockFromConsistent(inputDetails, outputDetails, nInputHeight))
        return false;
    // Fail count must be incremented appropriately
    uint64_t compFailCount = inputDetails.failCount;
    IncrementWitnessFailCount(compFailCount);
    if (compFailCount == outputDetails.failCount)
        return true;
    return false;
}

/*
* 5) Increase amount and/or lock period to increase, reset lockfrom to 0. Lock period and/or amount must exceed or equal old period/amount.
* Input/Output.
* Signed by spending key.
*/
inline bool IsIncreaseBundle(const CTxIn& input, const CTxOutPoW2Witness& inputDetails, const CTxOutPoW2Witness& outputDetails, CAmount nInputAmount, CAmount nOutputAmount, uint64_t nInputHeight)
{
    // Needs 2 signature (spending key)
    if (input.segregatedSignatureData.stack.size() != 2)
        return false;
    // Keys unchanged.
    if (inputDetails.spendingKeyID != outputDetails.spendingKeyID)
        return false;
    if (inputDetails.witnessKeyID != outputDetails.witnessKeyID)
        return false;
    if (inputDetails.failCount != outputDetails.failCount)
        return false;
    // Amount must have increased or lock until must have increased
    if (nInputAmount >= nOutputAmount && inputDetails.lockUntilBlock >= outputDetails.lockUntilBlock)
        return false;
    //fixme: (2.0) - High check the WEIGHT as well.
    // Lock until block may never decrease
    if (inputDetails.lockUntilBlock > outputDetails.lockUntilBlock)
        return false;
    // Lock from block resets to 0
    if (outputDetails.lockFromBlock != 0)
        return false;
    return true;
}

/*
* 6) Split, everything must stay the same, but input may be split into two identical outputs.
* 1 input, multiple outputs.
* Signed by witness key.
* Precondition - we must have already verified externally from here that the scriptwitness stack for the input is of size 2.
*/
bool CWitnessTxBundle::IsValidSplitBundle()
{
    if (inputs.size() != 1)
        return false;
    if (outputs.size() < 2)
        return false;
    const auto& input = inputs[0];
    const auto& inputDetails = input.second;
    CAmount nInputValue = input.first.nValue;
    CAmount nOutputValue = 0;
    for (const auto& output : outputs)
    {
        const auto& outputDetails = output.second;
        if (inputDetails.spendingKeyID != outputDetails.spendingKeyID)
            return false;
        if (inputDetails.witnessKeyID != outputDetails.witnessKeyID)
            return false;
        if (inputDetails.failCount != outputDetails.failCount)
            return false;
        if (inputDetails.lockUntilBlock != outputDetails.lockUntilBlock)
            return false;
        if (inputDetails.lockFromBlock != outputDetails.lockFromBlock)
            return false;
        nOutputValue += output.first.nValue;
    }
    if (nInputValue != nOutputValue)
        return false;
    return true;
}

/*
* 7) Merge, two identical (other than amount and failcount) inputs can be combined into one output. Everything should stay the same except amount and failcount which must match the combination of the two inputs in both cases.
* Multiple inputs, one output.
* Signed by witness key.
* Precondition - we must have already verified externally from here that the scriptwitness stack for the input is of size 2.
*/
bool CWitnessTxBundle::IsValidMergeBundle()
{
    if (outputs.size() != 1)
        return false;
    if (inputs.size() < 2)
        return false;

    const auto& output = outputs[0];
    const auto& outputDetails = output.second;
    CAmount nOutputValue = output.first.nValue;
    CAmount nInputValue = 0;
    for (const auto& input : inputs)
    {
        const auto& inputDetails = input.second;
        if (inputDetails.spendingKeyID != outputDetails.spendingKeyID)
            return false;
        if (inputDetails.witnessKeyID != outputDetails.witnessKeyID)
            return false;
        if (inputDetails.failCount != outputDetails.failCount)
            return false;
        if (inputDetails.lockUntilBlock != outputDetails.lockUntilBlock)
            return false;
        if (inputDetails.lockFromBlock != outputDetails.lockFromBlock)
            return false;
        nInputValue += input.first.nValue;
    }
    if (nInputValue != nOutputValue)
        return false;
    return true;
}

/*
* 8) Update, identical input/output but witness key changes.
* Input/Output.
* Signed by spending key.
*/
inline bool IsChangeWitnessKeyBundle(const CTxIn& input, const CTxOutPoW2Witness& inputDetails, const CTxOutPoW2Witness& outputDetails, CAmount nInputAmount, CAmount nOutputAmount, uint64_t nInputHeight)
{
    // 2 signatures (spending key)
    if (input.segregatedSignatureData.stack.size() != 2)
        return false;
    // Everything unchanged except witness key.
    if (nInputAmount != nOutputAmount)
        return false;
    if (inputDetails.spendingKeyID != outputDetails.spendingKeyID)
        return false;
    if (inputDetails.lockUntilBlock != outputDetails.lockUntilBlock)
        return false;
    if (inputDetails.lockFromBlock != outputDetails.lockFromBlock)
        return false;
    if (inputDetails.failCount != outputDetails.failCount)
        return false;
    if (inputDetails.witnessKeyID == outputDetails.witnessKeyID)
        return false;
    return true;
}

//fixme: (2.0) (HIGH) Implement test code for this function.
bool CheckTxInputAgainstWitnessBundles(CValidationState& state, std::vector<CWitnessTxBundle>* pWitnessBundles, const CTxOut& prevOut, const CTxIn input, uint64_t nInputHeight)
{
    if (pWitnessBundles)
    {
        if (IsPow2WitnessOutput(prevOut))
        {
            CTxOutPoW2Witness inputDetails;
            GetPow2WitnessOutput(prevOut, inputDetails);
            // At this point all bundles are currently marked either Creation/Witness - modify the types as we pair them up.
            bool matchedExistingBundle = false;
            for (auto& bundle : *pWitnessBundles)
            {
                if (bundle.outputs.size() == 1)
                {
                    const auto& outputDetails = bundle.outputs[0].second;

                    // Witnessing: amount should stay the same or increase, fail count can reduced by 1 or 0, if lockfrom was previously 0 it must be set to the current block height. Can only appear as a "witnesscoinbase" transaction.
                    if ( IsWitnessBundle(input, inputDetails, outputDetails, prevOut.nValue, bundle.outputs[0].first.nValue, nInputHeight) )
                    {
                        matchedExistingBundle = true;
                        bundle.inputs.push_back(std::make_pair(prevOut, std::move(inputDetails)));
                        bundle.bundleType = CWitnessTxBundle::WitnessTxType::WitnessType;
                        break;
                    }
                    else if ( IsRenewalBundle(input, inputDetails, outputDetails, prevOut.nValue, bundle.outputs[0].first.nValue, nInputHeight) )
                    {
                        matchedExistingBundle = true;
                        bundle.inputs.push_back(std::make_pair(prevOut, std::move(inputDetails)));
                        bundle.bundleType = CWitnessTxBundle::WitnessTxType::RenewType;
                        break;
                    }
                    else if ( IsIncreaseBundle(input, inputDetails, outputDetails, prevOut.nValue, bundle.outputs[0].first.nValue, nInputHeight) )
                    {
                        matchedExistingBundle = true;
                        bundle.inputs.push_back(std::make_pair(prevOut, std::move(inputDetails)));
                        bundle.bundleType = CWitnessTxBundle::WitnessTxType::IncreaseType;
                        break;
                    }
                    else if ( IsChangeWitnessKeyBundle(input, inputDetails, outputDetails, prevOut.nValue, bundle.outputs[0].first.nValue, nInputHeight) )
                    {
                        matchedExistingBundle = true;
                        bundle.inputs.push_back(std::make_pair(prevOut, std::move(inputDetails)));
                        bundle.bundleType = CWitnessTxBundle::WitnessTxType::ChangeWitnessKeyType;
                        break;
                    }
                }
            }
            if (!matchedExistingBundle)
            {
                //NB! We -must- check here that we have the spending key (2 items on stack) as when we later check the built up MergeType/SplitType bundles we have no way to check it then.
                //So this check is very important, must not be skipped and must come before the bundle creation for these bundle types.
                if (!HasSpendKey(input, inputDetails))
                {
                    return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-witness-input-without-matching-output");
                }

                bool matchedExistingBundle = false;
                for (auto& bundle : *pWitnessBundles)
                {
                    if ( (bundle.bundleType == CWitnessTxBundle::WitnessTxType::MergeType) && (bundle.outputs[0].second.witnessKeyID == inputDetails.witnessKeyID) && (bundle.outputs[0].second.spendingKeyID == inputDetails.spendingKeyID) )
                    {
                         bundle.inputs.push_back(std::make_pair(prevOut, std::move(inputDetails)));
                    }
                    if ( (bundle.bundleType == CWitnessTxBundle::WitnessTxType::SplitType) && (bundle.outputs[0].second.witnessKeyID == inputDetails.witnessKeyID) && (bundle.outputs[0].second.spendingKeyID == inputDetails.spendingKeyID) )
                    {
                        bundle.inputs.push_back(std::make_pair(prevOut, std::move(inputDetails)));
                    }
                }
                if (!matchedExistingBundle)
                {
                    //fixme: (2.0) (HIGH) ensure not still locked nLockFrom.
                    CWitnessTxBundle spendBundle = CWitnessTxBundle(CWitnessTxBundle::WitnessTxType::SpendType);
                    spendBundle.inputs.push_back(std::make_pair(prevOut, std::move(inputDetails)));
                    pWitnessBundles->push_back(spendBundle);
                }
            }
        }
    }
    return true;
}

bool Consensus::CheckTxInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, int nSpendHeight, std::vector<CWitnessTxBundle>* pWitnessBundles)
{
        // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
        // for an attacker to attempt to split the network.
        if (tx.IsPoW2WitnessCoinBase())
        {
            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                if (!tx.vin[i].prevout.IsNull())
                {
                    if (!inputs.HaveCoin(tx.vin[i].prevout))
                    {
                        return state.Invalid(false, 0, "", "Inputs unavailable");
                    }
                }
            }
        }
        else
        {
            if (!inputs.HaveInputs(tx))
                return state.Invalid(false, 0, "", "Inputs unavailable");
        }

        CAmount nValueIn = 0;
        CAmount nFees = 0;
        for (unsigned int i = 0; i < tx.vin.size(); i++)
        {
            const COutPoint &prevout = tx.vin[i].prevout;
            if (prevout.IsNull() && tx.IsPoW2WitnessCoinBase())
                continue;
            const Coin& coin = inputs.AccessCoin(prevout);
            assert(!coin.IsSpent());

            // If prev is coinbase, check that it's matured
            if (coin.IsCoinBase()) {
                if (nSpendHeight - coin.nHeight < COINBASE_MATURITY)
                    return state.Invalid(false,
                        REJECT_INVALID, "bad-txns-premature-spend-of-coinbase",
                        strprintf("tried to spend coinbase at depth %d", nSpendHeight - coin.nHeight));
            }

            // Check for negative or overflow input values
            nValueIn += coin.out.nValue;
            if (!MoneyRange(coin.out.nValue) || !MoneyRange(nValueIn))
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputvalues-outofrange");

            if (pWitnessBundles)
            {
                if (!CheckTxInputAgainstWitnessBundles(state, pWitnessBundles, coin.out, tx.vin[i], coin.nHeight))
                    return false;
            }
        }
        CAmount witnessPenaltyFee = 0;
        if (pWitnessBundles)
        {
            for (auto& bundle : *pWitnessBundles)
            {
                if (bundle.bundleType == CWitnessTxBundle::WitnessTxType::SplitType)
                {
                    if (!bundle.IsValidSplitBundle())
                        return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-witness-invalid-split-bundle");
                }
                else if(bundle.bundleType == CWitnessTxBundle::WitnessTxType::MergeType)
                {
                    if (!bundle.IsValidMergeBundle())
                        return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-witness-invalid-merge-bundle");
                }
                else if(bundle.bundleType == CWitnessTxBundle::WitnessTxType::SpendType)
                {
                    if (!bundle.IsValidSpendBundle(nSpendHeight))
                        return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-witness-invalid-spend-bundle");
                }
                else if(bundle.bundleType == CWitnessTxBundle::WitnessTxType::RenewType)
                {
                    witnessPenaltyFee += CalculateWitnessPenaltyFee(bundle.outputs[0].first);
                }
            }
        }

        if (nValueIn < tx.GetValueOut())
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-belowout", false,
                strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn), FormatMoney(tx.GetValueOut())));

        // Tally transaction fees
        CAmount nTxFee = nValueIn - tx.GetValueOut();
        if (nTxFee < 0)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-negative");
        if (nTxFee < witnessPenaltyFee)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-missing-witness-renewal-penalty-fee");
        nFees += nTxFee;
        if (!MoneyRange(nFees))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-outofrange");
    return true;
}
