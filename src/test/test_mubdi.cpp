/***********************************************************************
********Copyright (c) 2011-2013 The Bitcoin Core developers*************
*************Copyright (c) 2017-2020 The PIVX developers****************
******************Copyright (c) 2010-2023 Nur1Labs**********************
>Distributed under the MIT/X11 software license, see the accompanying
>file COPYING or http://www.opensource.org/licenses/mit-license.php.
************************************************************************/

#define BOOST_TEST_MODULE MuBdI Test Suite

#include "test/test_mubdi.h"

#include "blockassembler.h"
#include "consensus/merkle.h"
#include "bls/bls_wrapper.h"
#include "guiinterface.h"
#include "evo/evodb.h"
#include "evo/evonotificationinterface.h"
#include "miner.h"
#include "net_processing.h"
#include "rpc/server.h"
#include "rpc/register.h"
#include "pow.h"
#include "script/sigcache.h"
#include "sporkdb.h"
#include "txmempool.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

std::unique_ptr<CConnman> g_connman;

CClientUIInterface uiInterface;  // Declared but not defined in guiinterface.h

FastRandomContext insecure_rand_ctx;

extern bool fPrintToConsole;
extern void noui_connect();

std::ostream& operator<<(std::ostream& os, const uint256& num)
{
    os << num.ToString();
    return os;
}

BasicTestingSetup::BasicTestingSetup(const std::string& chainName)
    : m_path_root(fs::temp_directory_path() / "test_mubdi" / strprintf("%lu_%i", (unsigned long)GetTime(), (int)(InsecureRandRange(1 << 30))))
{
    ECC_Start();
    BLSInit();
    SetupEnvironment();
    InitSignatureCache();
    fCheckBlockIndex = true;
    SelectParams(chainName);
    evoDb.reset(new CEvoDB(1 << 20, true, true));
}

BasicTestingSetup::~BasicTestingSetup()
{
    fs::remove_all(m_path_root);
    ECC_Stop();
    evoDb.reset();
}

fs::path BasicTestingSetup::SetDataDir(const std::string& name)
{
    fs::path ret = m_path_root / name;
    fs::create_directories(ret);
    gArgs.ForceSetArg("-datadir", ret.string());
    return ret;
}

TestingSetup::TestingSetup(const std::string& chainName) : BasicTestingSetup(chainName)
{
        SetDataDir("tempdir");
        ClearDatadirCache();

        // Start the lightweight task scheduler thread
        CScheduler::Function serviceLoop = std::bind(&CScheduler::serviceQueue, &scheduler);
        threadGroup.create_thread(std::bind(&TraceThread<CScheduler::Function>, "scheduler", serviceLoop));

        // Note that because we don't bother running a scheduler thread here,
        // callbacks via CValidationInterface are unreliable, but that's OK,
        // our unit tests aren't testing multiple parts of the code at once.
        GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);

        g_connman = std::make_unique<CConnman>(0x1337, 0x1337); // Deterministic randomness for tests.
        connman = g_connman.get();

        // Register EvoNotificationInterface
        pEvoNotificationInterface = new EvoNotificationInterface();
        RegisterValidationInterface(pEvoNotificationInterface);

        // Ideally we'd move all the RPC tests to the functional testing framework
        // instead of unit tests, but for now we need these here.
        RegisterAllCoreRPCCommands(tableRPC);
        pSporkDB.reset(new CSporkDB(0, true));
        pblocktree.reset(new CBlockTreeDB(1 << 20, true));
        pcoinsdbview.reset(new CCoinsViewDB(1 << 23, true));
        pcoinsTip.reset(new CCoinsViewCache(pcoinsdbview.get()));
        if (!LoadGenesisBlock()) {
            throw std::runtime_error("Error initializing block database");
        }
        {
            CValidationState state;
            bool ok = ActivateBestChain(state);
            BOOST_CHECK(ok);
        }
        nScriptCheckThreads = 3;
        for (int i=0; i < nScriptCheckThreads-1; i++)
            threadGroup.create_thread(&ThreadScriptCheck);
        peerLogic.reset(new PeerLogicValidation(connman));
}

TestingSetup::~TestingSetup()
{
        scheduler.stop();
        threadGroup.interrupt_all();
        threadGroup.join_all();
        GetMainSignals().FlushBackgroundCallbacks();
        UnregisterAllValidationInterfaces();
        GetMainSignals().UnregisterBackgroundSignalScheduler();
        g_connman.reset();
        peerLogic.reset();
        UnloadBlockIndex();
        delete pEvoNotificationInterface;
        pcoinsTip.reset();
        pcoinsdbview.reset();
        pblocktree.reset();
        pSporkDB.reset();
}

TestChainSetup::~TestChainSetup()
{
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CMutableTransaction& tx)
{
    CTransaction txn(tx);
    return FromTx(txn);
}

CTxMemPoolEntry TestMemPoolEntryHelper::FromTx(const CTransaction& txn)
{
    CAmount inChainValue = 0;
    return CTxMemPoolEntry(MakeTransactionRef(txn), nFee, nTime, dPriority, nHeight,
                          inChainValue, spendsCoinbaseOrCoinstake, sigOpCount);
}

[[noreturn]] void Shutdown(void* parg)
{
    std::exit(0);
}

[[noreturn]] void StartShutdown()
{
    std::exit(0);
}

bool ShutdownRequested()
{
  return false;
}
