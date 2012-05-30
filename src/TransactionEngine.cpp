#include "TransactionEngine.h"

#include "Config.h"
#include "TransactionFormats.h"
#include "utils.h"

#include <boost/format.hpp>

typedef SerializedLedgerEntry SLE;

#define DIR_NODE_MAX	32

// We return the uNodeDir so that on delete we can quickly know where the element is mentioned in the directory.
TransactionEngineResult TransactionEngine::dirAdd(
	std::vector<AffectedAccount>&	accounts,
	uint64&							uNodeDir,
	const LedgerEntryType			letKind,
	const uint256&					uBase,
	const uint256&					uLedgerIndex)
{
	// Get the root.
	uint256				uRootIndex	= Ledger::getDirIndex(uBase, letKind);
	LedgerStateParms	lspRoot		= lepNONE;
	SLE::pointer		sleRoot		= mLedger->getDirRoot(lspRoot, uRootIndex);
	bool				bRootNew;

	// Get the last node index.
	if (sleRoot)
	{
		bRootNew	= false;
		uNodeDir	= sleRoot->getIFieldU64(sfLastNode);
	}
	else
	{
		bRootNew	= true;
		uNodeDir	= 1;

		sleRoot	= boost::make_shared<SerializedLedgerEntry>(ltDIR_ROOT);


		sleRoot->setIndex(uRootIndex);
		std::cerr << "dirAdd: Creating dir index: " << sleRoot->getIndex().ToString() << std::endl;

		sleRoot->setIFieldU64(sfFirstNode, uNodeDir);
		sleRoot->setIFieldU64(sfLastNode, uNodeDir);

		std::cerr << "dirAdd: first & last: " << strHex(uNodeDir) << std::endl;

		accounts.push_back(std::make_pair(taaCREATE, sleRoot));
	}

	// Get the last node.
	uint256				uNodeIndex	= Ledger::getDirIndex(uBase, letKind, uNodeDir);
	LedgerStateParms	lspNode		= lepNONE;
	SLE::pointer		sleNode		= bRootNew ? SLE::pointer() : mLedger->getDirNode(lspNode, uNodeIndex);

	if (sleNode)
	{
		STVector256	svIndexes;

		svIndexes	= sleNode->getIFieldV256(sfIndexes);
		if (DIR_NODE_MAX != svIndexes.peekValue().size())
		{
			// Last node is not full, append.

			svIndexes.peekValue().push_back(uLedgerIndex);
			sleNode->setIFieldV256(sfIndexes, svIndexes);

			accounts.push_back(std::make_pair(taaMODIFY, sleNode));
		}
		// Last node is full, add a new node.
		else if (!++uNodeDir)
		{
			return terDIR_FULL;
		}
		else
		{
			// Record new last node.
			sleNode	= SLE::pointer();

			std::cerr << "dirAdd:  last: " << strHex(uNodeDir) << std::endl;

			sleRoot->setIFieldU64(sfLastNode, uNodeDir);
			accounts.push_back(std::make_pair(taaMODIFY, sleRoot));
		}
	}

	if (!sleNode)
	{
		// Add to last node, which is empty.
		sleNode	= boost::make_shared<SerializedLedgerEntry>(ltDIR_NODE);
		sleNode->setIndex(uNodeIndex);

		std::cerr << "dirAdd: Creating dir node: " << sleNode->getIndex().ToString() << std::endl;

		STVector256	svIndexes;

		svIndexes.peekValue().push_back(uLedgerIndex);
		sleNode->setIFieldV256(sfIndexes, svIndexes);

		accounts.push_back(std::make_pair(taaCREATE, sleNode));
	}

	return terSUCCESS;
}

TransactionEngineResult TransactionEngine::dirDelete(
	std::vector<AffectedAccount>&	accounts,
	const uint64&					uNodeDir,
	const LedgerEntryType			letKind,
	const uint256&					uBase,
	const uint256&					uLedgerIndex)
{
	uint64				uNodeCur	= uNodeDir;
	uint256				uNodeIndex	= Ledger::getDirIndex(uBase, letKind, uNodeCur);
	LedgerStateParms	lspNode		= lepNONE;
	SLE::pointer		sleNode		= mLedger->getDirNode(lspNode, uNodeIndex);

	if (!sleNode)
	{
		std::cerr << "dirDelete: no such node" << std::endl;
		return terNODE_NOT_FOUND;
	}
	else
	{
		STVector256						svIndexes	= sleNode->getIFieldV256(sfIndexes);
		std::vector<uint256>&			vuiIndexes	= svIndexes.peekValue();
		std::vector<uint256>::iterator	it;

		it = std::find(vuiIndexes.begin(), vuiIndexes.end(), uLedgerIndex);
		if (vuiIndexes.end() == it)
		{
			std::cerr << "dirDelete: node not mentioned" << std::endl;
			return terNODE_NOT_MENTIONED;
		}
		else
		{
			// Get root information
			LedgerStateParms	lspRoot		= lepNONE;
			SLE::pointer		sleRoot		= mLedger->getDirRoot(lspRoot, Ledger::getDirIndex(uBase, letKind));

			if (!sleRoot)
			{
				std::cerr << "dirDelete: root node is missing" << std::endl;
				return terNODE_NO_ROOT;
			}

			uint64	uFirstNodeOrig	= sleRoot->getIFieldU64(sfFirstNode);
			uint64	uLastNodeOrig	= sleRoot->getIFieldU64(sfLastNode);
			uint64	uFirstNode		= uFirstNodeOrig;
			uint64	uLastNode		= uLastNodeOrig;

			// Remove the element.
			if (vuiIndexes.size() > 1)
			{
				*it = vuiIndexes[vuiIndexes.size()-1];
			}
			vuiIndexes.resize(vuiIndexes.size()-1);

			sleNode->setIFieldV256(sfIndexes, svIndexes);

			if (!vuiIndexes.empty() || (uFirstNode != uNodeCur && uLastNode != uNodeCur))
			{
				// Node is not being deleted.
				accounts.push_back(std::make_pair(taaMODIFY, sleNode));
			}

			while (uFirstNode && svIndexes.peekValue().empty()
				&& (uFirstNode == uNodeCur || uLastNode == uNodeCur))
			{
				// Current node is empty and first or last, delete it.
				accounts.push_back(std::make_pair(taaDELETE, sleNode));

				if (uFirstNode == uLastNode)
				{
					// Complete deletion.
					uFirstNode	= 0;
				}
				else
				{
					if (uFirstNode == uNodeCur)
					{
						// Advance first node
						++uNodeCur;
						++uFirstNode;
					}
					else
					{
						// Rewind last node
						--uNodeCur;
						--uLastNode;
					}

					// Get replacement node.
					lspNode		= lepNONE;
					sleNode		= mLedger->getDirNode(lspNode, Ledger::getDirIndex(uBase, letKind, uNodeCur));
					svIndexes	= sleNode->getIFieldV256(sfIndexes);
				}
			}

			if (uFirstNode == uFirstNodeOrig && uLastNode == uLastNodeOrig)
			{
				// Dir is fine.
				nothing();
			}
			else if (uFirstNode)
			{
				// Update root's pointer pointers.
				sleRoot->setIFieldU64(sfFirstNode, uFirstNode);
				sleRoot->setIFieldU64(sfLastNode, uLastNode);

				accounts.push_back(std::make_pair(taaMODIFY, sleRoot));
			}
			else
			{
				// Delete the root.
				accounts.push_back(std::make_pair(taaDELETE, sleRoot));
			}
		}

		return terSUCCESS;
	}
}

TransactionEngineResult TransactionEngine::applyTransaction(const SerializedTransaction& txn,
	TransactionEngineParams params)
{
	std::cerr << "applyTransaction>" << std::endl;

	TransactionEngineResult result = terSUCCESS;

	uint256 txID = txn.getTransactionID();
	if (!txID)
	{
		std::cerr << "applyTransaction: invalid transaction id" << std::endl;
		return tenINVALID;
	}

	// Extract signing key
	// Transactions contain a signing key.  This allows us to trivially verify a transaction has at least been properly signed
	// without going to disk.  Each transaction also notes a source account id.  This is used to verify that the signing key is
	// associated with the account.
	// XXX This could be a lot cleaner to prevent unnecessary copying.
	NewcoinAddress	naPubKey;

	naPubKey.setAccountPublic(txn.peekSigningPubKey());

	// check signature
	if (!txn.checkSign(naPubKey))
	{
		std::cerr << "applyTransaction: Invalid transaction: bad signature" << std::endl;
		return tenINVALID;
	}

	STAmount	saCost		= theConfig.FEE_DEFAULT;

	// Customize behavoir based on transaction type.
	switch(txn.getTxnType())
	{
		case ttCLAIM:
			saCost	= 0;
			break;

		case ttPAYMENT:
			if (txn.getFlags() & tfCreateAccount)
			{
				saCost	= theConfig.FEE_CREATE;
			}
			break;

		case ttINVOICE:
		case ttOFFER:
		case ttCREDIT_SET:
		case ttTRANSIT_SET:
			result = terSUCCESS;
			break;

		case ttINVALID:
			std::cerr << "applyTransaction: Invalid transaction: ttINVALID transaction type" << std::endl;
			result = tenINVALID;
			break;

		default:
			std::cerr << "applyTransaction: Invalid transaction: unknown transaction type" << std::endl;
			result = tenUNKNOWN;
			break;
	}

	if (terSUCCESS != result)
		return result;

	STAmount saPaid = txn.getTransactionFee();
	if ( (params & tepNO_CHECK_FEE) != tepNONE)
	{
		if (saCost)
		{
			if (saPaid < saCost)
			{
				std::cerr << "applyTransaction: insufficient fee" << std::endl;
				return tenINSUF_FEE_P;
			}
		}
		else
		{
			if (!saPaid.isZero())
			{
				// Transaction is malformed.
				std::cerr << "applyTransaction: fee not allowed" << std::endl;
				return tenINSUF_FEE_P;
			}
		}
	}

	// Get source account ID.
	uint160 srcAccountID = txn.getSourceAccount().getAccountID();
	if (!srcAccountID)
	{
		std::cerr << "applyTransaction: bad source id" << std::endl;
		return tenINVALID;
	}

	boost::recursive_mutex::scoped_lock sl(mLedger->mLock);

	// find source account
	// If we are only verifying some transactions, this would be probablistic.
	LedgerStateParms	lspRoot	= lepNONE;
	SLE::pointer		sleSrc	= mLedger->getAccountRoot(lspRoot, srcAccountID);
	if (!sleSrc)
	{
		std::cerr << str(boost::format("applyTransaction: Delay transaction: source account does not exisit: %s") % txn.getSourceAccount().humanAccountID()) << std::endl;
		return terNO_ACCOUNT;
	}

	// deduct the fee, so it's not available during the transaction
	// we only write the account back if the transaction succeeds
	if (!saCost.isZero())
	{
		STAmount saSrcBalance = sleSrc->getIValueFieldAmount(sfBalance);

		if (saSrcBalance < saPaid)
		{
			std::cerr
				<< str(boost::format("applyTransaction: Delay transaction: insufficent balance: balance=%s paid=%s")
					% saSrcBalance
					% saPaid)
				<< std::endl;
			return terINSUF_FEE_B;
		}

		sleSrc->setIFieldAmount(sfBalance, saSrcBalance - saPaid);
	}

	// Validate sequence
	uint32 t_seq = txn.getSequence();

	if (saCost)
	{
		uint32 a_seq = sleSrc->getIFieldU32(sfSequence);

		if (t_seq != a_seq)
		{
			// WRITEME: Special case code for changing transaction key
			if (a_seq < t_seq)
			{
				std::cerr << "applyTransaction: future sequence number" << std::endl;
				return terPRE_SEQ;
			}
			if (mLedger->hasTransaction(txID))
			{
				std::cerr << "applyTransaction: duplicate sequence number" << std::endl;
				return terALREADY;
			}

			std::cerr << "applyTransaction: past sequence number" << std::endl;
			return terPAST_SEQ;
		}
		else sleSrc->setIFieldU32(sfSequence, t_seq);
	}
	else
	{
		if (t_seq)
		{
			std::cerr << "applyTransaction: bad sequence for pre-paid transaction" << std::endl;
			return terPAST_SEQ;
		}
	}

	std::vector<AffectedAccount> accounts;
	accounts.push_back(std::make_pair(taaMODIFY, sleSrc));

	switch(txn.getTxnType())
	{
		case ttCLAIM:
			result = doClaim(txn, accounts);
			break;

		case ttCREDIT_SET:
			result = doCreditSet(txn, accounts, srcAccountID);
			break;

		case ttINVALID:
			std::cerr << "applyTransaction: invalid type" << std::endl;
			result = tenINVALID;
			break;

		case ttINVOICE:
			result = doInvoice(txn, accounts);
			break;

		case ttOFFER:
			result = doOffer(txn, accounts);
			break;

		case ttPAYMENT:
			result = doPayment(txn, accounts, srcAccountID);
			break;

		case ttTRANSIT_SET:
			result = doTransitSet(txn, accounts);
			break;

		default:
			result = tenUNKNOWN;
			break;
	}

	if (result == terSUCCESS)
	{ // Write back the account states and add the transaction to the ledger
		// WRITEME: Special case code for changing transaction key
		for (std::vector<AffectedAccount>::iterator it = accounts.begin(), end = accounts.end();
			it != end; ++it)
		{
			if (it->first == taaCREATE)
			{
				if (mLedger->writeBack(lepCREATE, it->second) & lepERROR)
					assert(false);
			}
			else if (it->first == taaMODIFY)
			{
				if (mLedger->writeBack(lepNONE, it->second) & lepERROR)
					assert(false);
			}
			else if (it->first == taaDELETE)
			{
				if (!mLedger->peekAccountStateMap()->delItem(it->second->getIndex()))
					assert(false);
			}
		}

		Serializer s;
		txn.add(s);
		mLedger->addTransaction(txID, s, saPaid);
	}

	return result;
}

TransactionEngineResult TransactionEngine::doClaim(const SerializedTransaction& txn,
	 std::vector<AffectedAccount>& accounts)
{
	std::cerr << "doClaim>" << std::endl;
	NewcoinAddress				naSigningPubKey;

	naSigningPubKey.setAccountPublic(txn.peekSigningPubKey());

	uint160	sourceAccountID	= naSigningPubKey.getAccountID();

	if (sourceAccountID != txn.getSourceAccount().getAccountID())
	{
		// Signing Pub Key must be for Source Account ID.
		std::cerr << "sourceAccountID: " << naSigningPubKey.humanAccountID() << std::endl;
		std::cerr << "txn accountID: " << txn.getSourceAccount().humanAccountID() << std::endl;
		return tenINVALID;
	}

	LedgerStateParms	qry				= lepNONE;
	SLE::pointer		sleDst			= accounts[0].second;

	if (!sleDst)
	{
		// Source account does not exist.  Could succeed if it was created first.
		std::cerr << str(boost::format("doClaim: no such account: %s") % txn.getSourceAccount().humanAccountID()) << std::endl;
		return terNO_ACCOUNT;
	}

	std::cerr << str(boost::format("doClaim: %s") % sleDst->getFullText()) << std::endl;

	if (sleDst->getIFieldPresent(sfAuthorizedKey))
	{
		// Source account already claimed.
		std::cerr << "doClaim: source already claimed" << std::endl;
		return terCLAIMED;
	}

	//
	// Verify claim is authorized for public key.
	//

	std::vector<unsigned char>	vucCipher		= txn.getITFieldVL(sfGenerator);
	std::vector<unsigned char>	vucPubKey		= txn.getITFieldVL(sfPubKey);
	std::vector<unsigned char>	vucSignature	= txn.getITFieldVL(sfSignature);

	NewcoinAddress				naAccountPublic;

	naAccountPublic.setAccountPublic(vucPubKey);

	if (!naAccountPublic.accountPublicVerify(Serializer::getSHA512Half(vucCipher), vucSignature))
	{
		std::cerr << "doClaim: bad signature unauthorized claim" << std::endl;
		return tenINVALID;
	}

	//
	// Verify generator not already in use.
	//

	uint160			hGeneratorID	= naAccountPublic.getAccountID();

					qry				= lepNONE;
	SLE::pointer	sleGen			= mLedger->getGenerator(qry, hGeneratorID);
	if (sleGen)
	{
		// Generator is already in use.  Regular passphrases limited to one wallet.
		std::cerr << "doClaim: generator already in use" << std::endl;
		return tenGEN_IN_USE;
	}

	//
	// Claim the account.
	//

	// Set the public key needed to use the account.
	sleDst->setIFieldH160(sfAuthorizedKey, hGeneratorID);

	// Construct a generator map entry.
					sleGen				= boost::make_shared<SerializedLedgerEntry>(ltGENERATOR_MAP);

	sleGen->setIndex(Ledger::getGeneratorIndex(hGeneratorID));
	sleGen->setIFieldH160(sfGeneratorID, hGeneratorID);
	sleGen->setIFieldVL(sfGenerator, vucCipher);

	accounts.push_back(std::make_pair(taaCREATE, sleGen));

	std::cerr << "doClaim<" << std::endl;

	return terSUCCESS;
}

TransactionEngineResult TransactionEngine::doCreditSet(const SerializedTransaction& txn,
	std::vector<AffectedAccount>&accounts,
	const uint160& uSrcAccountID)
{
	TransactionEngineResult	terResult	= terSUCCESS;
	std::cerr << "doCreditSet>" << std::endl;

	// Check if destination makes sense.
	uint160				uDstAccountID	= txn.getITFieldAccount(sfDestination);

	if (!uDstAccountID)
	{
		std::cerr << "doCreditSet: Invalid transaction: Payment destination account not specifed." << std::endl;
		return tenDST_NEEDED;
	}
	else if (uSrcAccountID == uDstAccountID)
	{
		std::cerr << "doCreditSet: Invalid transaction: Source account is the same as destination." << std::endl;
		return tenDST_IS_SRC;
	}

	LedgerStateParms	qry				= lepNONE;
	SLE::pointer		sleDst			= mLedger->getAccountRoot(qry, uDstAccountID);
	if (!sleDst)
	{
		std::cerr << "doCreditSet: Delay transaction: Destination account does not exist." << std::endl;

		return terNO_DST;
	}

	STAmount			saLimitAmount	= txn.getITFieldAmount(sfLimitAmount);
	uint160				uCurrency		= saLimitAmount.getCurrency();
	bool				bSltD			= uSrcAccountID < uDstAccountID;
	uint32				uFlags			= bSltD ? lsfLowIndexed : lsfHighIndexed;
	STAmount			saBalance(uCurrency);
	bool				bAddIndex;

						qry				= lepNONE;
	SLE::pointer		sleRippleState	= mLedger->getRippleState(qry, uSrcAccountID, uDstAccountID, uCurrency);
	if (sleRippleState)
	{
		std::cerr << "doCreditSet: Modifying ripple line." << std::endl;

						bAddIndex		= !(sleRippleState->getFlags() & uFlags);

		sleRippleState->setIFieldAmount(bSltD ? sfLowLimit : sfHighID, saLimitAmount);

		accounts.push_back(std::make_pair(taaMODIFY, sleRippleState));

		if (bAddIndex)
			sleRippleState->setFlag(uFlags);
	}
	// Line does not exist.
	else if (saLimitAmount.isZero())
	{
		std::cerr << "doCreditSet: Setting non-existant ripple line to 0." << std::endl;

		return terNO_LINE_NO_ZERO;
	}
	else
	{
		STAmount		saZero(uCurrency);

						bAddIndex		= true;
						sleRippleState	= boost::make_shared<SerializedLedgerEntry>(ltRIPPLE_STATE);

		sleRippleState->setIndex(Ledger::getRippleStateIndex(uSrcAccountID, uDstAccountID, uCurrency));
		std::cerr << "doCreditSet: Creating ripple line: "
			<< sleRippleState->getIndex().ToString()
			<< std::endl;

		sleRippleState->setFlag(uFlags);
		sleRippleState->setIFieldAmount(sfBalance, saZero);	// Zero balance in currency.
		sleRippleState->setIFieldAmount(bSltD ? sfLowLimit : sfHighLimit, saLimitAmount);
		sleRippleState->setIFieldAmount(bSltD ? sfHighLimit : sfLowLimit, saZero);
		sleRippleState->setIFieldAccount(bSltD ? sfLowID : sfHighID, uSrcAccountID);
		sleRippleState->setIFieldAccount(bSltD ? sfHighID : sfLowID, uDstAccountID);

		accounts.push_back(std::make_pair(taaCREATE, sleRippleState));
	}

	if (bAddIndex)
	{
		// Add entries so clients can find lines.
		// - Client needs to be able to walk who account has given credit to and who has account's credit.
		// - Client doesn't need to know every account who has extended credit but it owed nothing.
		uint64			uSrcRef;	// Ignored, ripple_state dirs never delete.

		// XXX Verify extend is passing the right bits, not the zero bits.
		// XXX Make dirAdd more flexiable to take vector.
		terResult	= dirAdd(accounts, uSrcRef, ltRIPPLE_STATE, uint160extend256(uSrcAccountID, 0), sleRippleState->getIndex());
	}

	std::cerr << "doCreditSet<" << std::endl;

	return terResult;
}

TransactionEngineResult TransactionEngine::doPayment(const SerializedTransaction& txn,
	std::vector<AffectedAccount>& accounts,
	const uint160& srcAccountID)
{
	uint32	txFlags			= txn.getFlags();
	uint160 uDstAccountID	= txn.getITFieldAccount(sfDestination);

	if (!uDstAccountID)
	{
		std::cerr << "doPayment: Invalid transaction: Payment destination account not specifed." << std::endl;
		return tenINVALID;
	}
	// XXX Only bad if no currency conversion in between through other people's offer.
	else if (srcAccountID == uDstAccountID)
	{
		std::cerr << "doPayment: Invalid transaction: Source account is the same as destination." << std::endl;
		return tenINVALID;
	}

	bool	bCreate	= !!(txFlags & tfCreateAccount);

	uint160	uCurrency;
	if (txn.getITFieldPresent(sfCurrency))
	{
		uCurrency = txn.getITFieldH160(sfCurrency);
		if (!uCurrency)
		{
			std::cerr << "doPayment: Invalid transaction: " SYSTEM_CURRENCY_CODE " explicitly specified." << std::endl;
			return tenEXPLICITXNC;
		}
	}

	LedgerStateParms	qry		= lepNONE;
	SLE::pointer		sleDst	= mLedger->getAccountRoot(qry, uDstAccountID);
	if (!sleDst)
	{
		// Destination account does not exist.
		if (bCreate && !!uCurrency)
		{
			std::cerr << "doPayment: Invalid transaction: Create account may only fund XBC." << std::endl;
			return tenCREATEXNC;
		}
		else if (!bCreate)
		{
			std::cerr << "doPayment: Delay transaction: Destination account does not exist." << std::endl;
			return terNO_DST;
		}

		// Create the account.
		sleDst = boost::make_shared<SerializedLedgerEntry>(ltACCOUNT_ROOT);

		sleDst->setIndex(Ledger::getAccountRootIndex(uDstAccountID));
		sleDst->setIFieldAccount(sfAccount, uDstAccountID);
		sleDst->setIFieldU32(sfSequence, 1);

		accounts.push_back(std::make_pair(taaCREATE, sleDst));
	}
	// Destination exists.
	else if (bCreate)
	{
		std::cerr << "doPayment: Invalid transaction: Account already created." << std::endl;
		return terCREATED;
	}
	else
	{
		accounts.push_back(std::make_pair(taaMODIFY, sleDst));
	}

	STAmount	saAmount = txn.getITFieldAmount(sfAmount);

	if (!uCurrency)
	{
		STAmount	saSrcBalance = accounts[0].second->getIValueFieldAmount(sfBalance);

		if (saSrcBalance < saAmount)
		{
			std::cerr << "doPayment: Delay transaction: Insufficent funds." << std::endl;
			return terUNFUNDED;
		}

		accounts[0].second->setIFieldAmount(sfBalance, saSrcBalance - saAmount);
		accounts[1].second->setIFieldAmount(sfBalance, accounts[1].second->getIValueFieldAmount(sfBalance) + saAmount);
	}
	else
	{
		// WRITEME: Handle non-native currencies, paths
		return tenUNKNOWN;
	}

	return terSUCCESS;
}

TransactionEngineResult TransactionEngine::doTransitSet(const SerializedTransaction& st, std::vector<AffectedAccount>&)
{
	std::cerr << "doTransitSet>" << std::endl;
#if 0
	SLE::pointer	sleSrc	= accounts[0].second;

	bool	bTxnTransitRate			= st->getIFieldPresent(sfTransitRate);
	bool	bTxnTransitStart		= st->getIFieldPresent(sfTransitStart);
	bool	bTxnTransitExpire		= st->getIFieldPresent(sfTransitExpire);
	uint32	uTxnTransitRate			= bTxnTransitRate ? st->getIFieldU32(sfTransitRate) : 0;
	uint32	uTxnTransitStart		= bTxnTransitStart ? st->getIFieldU32(sfTransitStart) : 0;
	uint32	uTxnTransitExpire		= bTxnTransitExpire ? st->getIFieldU32(sfTransitExpire) : 0;

	bool	bActTransitRate			= sleSrc->getIFieldPresent(sfTransitRate);
	bool	bActTransitExpire		= sleSrc->getIFieldPresent(sfTransitExpire);
	bool	bActNextTransitRate		= sleSrc->getIFieldPresent(sfNextTransitRate);
	bool	bActNextTransitStart	= sleSrc->getIFieldPresent(sfNextTransitStart);
	bool	bActNextTransitExpire	= sleSrc->getIFieldPresent(sfNextTransitExpire);
	uint32	uActTransitRate			= bActTransitRate ? sleSrc->getIFieldU32(sfTransitRate) : 0;
	uint32	uActTransitExpire		= bActTransitExpire ? sleSrc->getIFieldU32(sfTransitExpire) : 0;
	uint32	uActNextTransitRate		= bActNextTransitRate ? sleSrc->getIFieldU32(sfNextTransitRate) : 0;
	uint32	uActNextTransitStart	= bActNextTransitStart ? sleSrc->getIFieldU32(sfNextTransitStart) : 0;
	uint32	uActNextTransitExpire	= bActNextTransitExpire ? sleSrc->getIFieldU32(sfNextTransitExpire) : 0;

	//
	// Update view
	//

	bool	bNoCurrent		= !bActTransitRate;
	bool	bCurrentExpired	=
		bActTransitExpire						// Current can expire
			&& bActNextTransitStart				// Have a replacement
			&& uActTransitExpire <= uLedger;	// Current is expired

	// Replace current with next if need.
	if (bNoCurrent								// No current.
		&& bActNextTransitRate					// Have next.
		&& uActNextTransitStart <= uLedger)		// Next has started.
	{
		// Make next current.
		uActTransitRate			= uActNextTransitRate;
		bActTransitExpire		= bActNextTransitStart;
		uActTransitExpire		= uActNextTransitExpire;

		// Remove next.
		uActNextTransitStart	= 0;
	}

	//
	// Determine new transaction deposition.
	//

	bool	bBetterThanCurrent =
		!no current
			|| (
				Expires same or later than current
				Start before or same as current
				Fee same or less than current
			)

	bool	bBetterThanNext =
		!no next
			|| (
				Expires same or later than next
				Start before or same as next
				Fee same or less than next
			)

	bool	bBetterThanBoth =
		bBetterThanCurrent && bBetterThanNext

	bool	bCurrentBlocks =
		!bBetterThanCurrent
		&& overlaps with current

	bool	bNextBlocks =
		!bBetterThanNext
		&& overlaps with next

	if (bBetterThanBoth)
	{
		// Erase both and install.

		// If not starting now, install as next.
	}
	else if (bCurrentBlocks || bNextBlocks)
	{
		// Invalid ignore
	}
	else if (bBetterThanCurrent)
	{
		// Install over current
	}
	else if (bBetterThanNext)
	{
		// Install over next
	}
	else
	{
		// Error.
	}

	return tenTRANSIT_WORSE;

	// Set current.
	uDstTransitRate			= uTxnTransitRate;
	uDstTransitExpire		= uTxnTransitExpire;	// 0 for never expire.

	// Set future.
	uDstNextTransitRate		= uTxnTransitRate;
	uDstNextTransitStart	= uTxnTransitStart;
	uDstNextTransitExpire	= uTxnTransitExpire;	// 0 for never expire.

	if (txn.getITFieldPresent(sfCurrency))
#endif
	std::cerr << "doTransitSet<" << std::endl;
	return tenINVALID;
}

TransactionEngineResult TransactionEngine::doInvoice(const SerializedTransaction& txn,
	std::vector<AffectedAccount>& accounts)
{
	return tenUNKNOWN;
}

TransactionEngineResult TransactionEngine::doOffer(const SerializedTransaction& txn,
	std::vector<AffectedAccount>& accounts)
{
	return tenUNKNOWN;
}

TransactionEngineResult TransactionEngine::doTake(const SerializedTransaction& txn,
	std::vector<AffectedAccount>& accounts)
{
	return tenUNKNOWN;
}

TransactionEngineResult TransactionEngine::doCancel(const SerializedTransaction& txn,
	 std::vector<AffectedAccount>& accounts)
{
	return tenUNKNOWN;
}

TransactionEngineResult TransactionEngine::doStore(const SerializedTransaction& txn,
	std::vector<AffectedAccount>& accounts)
{
	return tenUNKNOWN;
}

TransactionEngineResult TransactionEngine::doDelete(const SerializedTransaction& txn,
	std::vector<AffectedAccount>& accounts)
{
	return tenUNKNOWN;
}

// vim:ts=4
