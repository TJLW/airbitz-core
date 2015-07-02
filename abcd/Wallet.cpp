/*
 *  Copyright (c) 2014, Airbitz
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms are permitted provided that
 *  the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice, this
 *  list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *  3. Redistribution or use of modified source code requires the express written
 *  permission of Airbitz Inc.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  The views and conclusions contained in the software and documentation are those
 *  of the authors and should not be interpreted as representing official policies,
 *  either expressed or implied, of the Airbitz Project.
 */

#include "Wallet.hpp"
#include "Context.hpp"
#include "account/Account.hpp"
#include "crypto/Crypto.hpp"
#include "crypto/Encoding.hpp"
#include "json/JsonObject.hpp"
#include "util/FileIO.hpp"
#include "util/Json.hpp"
#include "util/Mutex.hpp"
#include "util/Util.hpp"
#include "wallet/Wallet.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

namespace abcd {

#define WALLET_NAME_FILENAME                    "WalletName.json"
#define WALLET_CURRENCY_FILENAME                "Currency.json"

#define JSON_WALLET_NAME_FIELD                  "walletName"
#define JSON_WALLET_CURRENCY_NUM_FIELD          "num"

struct WalletJson:
    public JsonObject
{
    ABC_JSON_CONSTRUCTORS(WalletJson, JsonObject)
    ABC_JSON_STRING(dataKey,    "MK",           nullptr)
    ABC_JSON_STRING(syncKey,    "SyncKey",      nullptr)
    ABC_JSON_STRING(bitcoinKey, "BitcoinSeed",  nullptr)
    // There are other fields, but the WalletList handles those.
};

// holds wallet data (including keys) for a given wallet
typedef struct sWalletData
{
    char            *szUUID;
    char            *szName;
    char            *szWalletAcctKey;
    int             currencyNum;
    tABC_U08Buf     MK;
    tABC_U08Buf     BitcoinPrivateSeed;
} tWalletData;

// this holds all the of the currently cached wallets
static unsigned int gWalletsCacheCount = 0;
static tWalletData **gaWalletsCacheArray = NULL;

static tABC_CC ABC_WalletCacheData(Wallet &self, tWalletData **ppData, tABC_Error *pError);
static tABC_CC ABC_WalletAddToCache(tWalletData *pData, tABC_Error *pError);
static tABC_CC ABC_WalletGetFromCacheByUUID(const char *szUUID, tWalletData **ppData, tABC_Error *pError);
static void    ABC_WalletFreeData(tWalletData *pData);

/**
 * Sets the name of a wallet
 */
tABC_CC ABC_WalletSetName(Wallet &self, const char *szName, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    tWalletData *pData = NULL;
    std::string path;
    char *szJSON = NULL;

    // load the wallet data into the cache
    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));

    // set the new name
    ABC_FREE_STR(pData->szName);
    ABC_STRDUP(pData->szName, szName);

    // create the json for the wallet name
    ABC_CHECK_RET(ABC_UtilCreateValueJSONString(szName, JSON_WALLET_NAME_FIELD, &szJSON, pError));
    //printf("name:\n%s\n", szJSON);

    // write the name out to the file
    path = self.syncDir() + WALLET_NAME_FILENAME;
    ABC_CHECK_RET(ABC_CryptoEncryptJSONFile(
        U08Buf((unsigned char *)szJSON, strlen(szJSON) + 1),
        pData->MK, ABC_CryptoType_AES256, path.c_str(), pError));

exit:
    ABC_FREE_STR(szJSON);

    return cc;
}

/**
 * Adds the wallet data to the cache
 * If the wallet is not currently in the cache it is added
 */
static
tABC_CC ABC_WalletCacheData(Wallet &self, tWalletData **ppData, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    tWalletData *pData = NULL;
    WalletJson json;
    DataChunk dataKey, bitcoinKey;

    // see if it is already in the cache
    ABC_CHECK_RET(ABC_WalletGetFromCacheByUUID(self.id().c_str(), &pData, pError));

    // if it is already cached
    if (NULL != pData)
    {
        // hang on to this data
        *ppData = pData;

        // if we don't do this, we'll free it below but it isn't ours, it is from the cache
        pData = NULL;
    }
    else
    {
        std::string syncDir = self.syncDir();
        // we need to add it

        // create a new wallet data struct
        ABC_NEW(pData, tWalletData);
        ABC_STRDUP(pData->szUUID, self.id().c_str());

        // Get the wallet info from the account:
        ABC_CHECK_NEW(self.account.wallets.json(json, self.id()));

        ABC_CHECK_NEW(json.dataKeyOk());
        ABC_CHECK_NEW(base16Decode(dataKey, json.dataKey()));
        ABC_BUF_DUP(pData->MK, toU08Buf(dataKey));

        ABC_CHECK_NEW(json.bitcoinKeyOk());
        ABC_CHECK_NEW(base16Decode(bitcoinKey, json.bitcoinKey()));
        ABC_BUF_DUP(pData->BitcoinPrivateSeed, toU08Buf(bitcoinKey));

        ABC_CHECK_NEW(json.syncKeyOk());
        ABC_STRDUP(pData->szWalletAcctKey, json.syncKey());

        // make sure this wallet exists, if it doesn't leave fields empty
        if (!fileExists(syncDir))
        {
            ABC_STRDUP(pData->szName, "");
            pData->currencyNum = -1;
        }
        else
        {
            std::string path;

            // get the name
            path = syncDir + WALLET_NAME_FILENAME;
            if (fileExists(path))
            {
                AutoU08Buf Data;
                ABC_CHECK_RET(ABC_CryptoDecryptJSONFile(path.c_str(), pData->MK, &Data, pError));
                ABC_CHECK_RET(ABC_UtilGetStringValueFromJSONString((char *)Data.data(), JSON_WALLET_NAME_FIELD, &(pData->szName), pError));
            }
            else
            {
                ABC_STRDUP(pData->szName, "");
            }

            // get the currency num
            path = syncDir + WALLET_CURRENCY_FILENAME;
            if (fileExists(path))
            {
                AutoU08Buf Data;
                ABC_CHECK_RET(ABC_CryptoDecryptJSONFile(path.c_str(), pData->MK, &Data, pError));
                ABC_CHECK_RET(ABC_UtilGetIntValueFromJSONString((char *)Data.data(), JSON_WALLET_CURRENCY_NUM_FIELD, (int *) &(pData->currencyNum), pError));
            }
            else
            {
                pData->currencyNum = -1;
            }
        }

        // Add to cache
        ABC_CHECK_RET(ABC_WalletAddToCache(pData, pError));

        // hang on to this data
        *ppData = pData;

        // if we don't do this, we'll free it below but it isn't ours, it is from the cache
        pData = NULL;
    }

exit:
    if (pData)
    {
        ABC_WalletFreeData(pData);
        ABC_CLEAR_FREE(pData, sizeof(tWalletData));
    }

    return cc;
}

/**
 * Clears all the data from the cache
 */
void ABC_WalletClearCache()
{
    AutoCoreLock lock(gCoreMutex);

    if ((gWalletsCacheCount > 0) && (NULL != gaWalletsCacheArray))
    {
        for (unsigned i = 0; i < gWalletsCacheCount; i++)
        {
            tWalletData *pData = gaWalletsCacheArray[i];
            ABC_WalletFreeData(pData);
        }

        ABC_FREE(gaWalletsCacheArray);
        gWalletsCacheCount = 0;
    }
}

/**
 * Adds the given WalletDAta to the array of cached wallets
 */
static
tABC_CC ABC_WalletAddToCache(tWalletData *pData, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    tWalletData *pExistingWalletData = NULL;

    ABC_CHECK_NULL(pData);

    // see if it exists first
    ABC_CHECK_RET(ABC_WalletGetFromCacheByUUID(pData->szUUID, &pExistingWalletData, pError));

    // if it doesn't currently exist in the array
    if (pExistingWalletData == NULL)
    {
        // if we don't have an array yet
        if ((gWalletsCacheCount == 0) || (NULL == gaWalletsCacheArray))
        {
            // create a new one
            gWalletsCacheCount = 0;
            ABC_ARRAY_NEW(gaWalletsCacheArray, 1, tWalletData*);
        }
        else
        {
            // extend the current one
            ABC_ARRAY_RESIZE(gaWalletsCacheArray, gWalletsCacheCount + 1, tWalletData*);
        }
        gaWalletsCacheArray[gWalletsCacheCount] = pData;
        gWalletsCacheCount++;
    }
    else
    {
        cc = ABC_CC_WalletAlreadyExists;
    }

exit:
    return cc;
}

/**
 * Searches for a wallet in the cached by UUID
 * if it is not found, the wallet data will be set to NULL
 */
static
tABC_CC ABC_WalletGetFromCacheByUUID(const char *szUUID, tWalletData **ppData, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    ABC_CHECK_NULL(szUUID);
    ABC_CHECK_NULL(ppData);

    // assume we didn't find it
    *ppData = NULL;

    if ((gWalletsCacheCount > 0) && (NULL != gaWalletsCacheArray))
    {
        for (unsigned i = 0; i < gWalletsCacheCount; i++)
        {
            tWalletData *pData = gaWalletsCacheArray[i];
            if (0 == strcmp(szUUID, pData->szUUID))
            {
                // found it
                *ppData = pData;
                break;
            }
        }
    }

exit:

    return cc;
}

/**
 * Free's the given wallet data elements
 */
static
void ABC_WalletFreeData(tWalletData *pData)
{
    if (pData)
    {
        ABC_FREE_STR(pData->szUUID);

        ABC_FREE_STR(pData->szName);

        pData->currencyNum = -1;

        ABC_BUF_FREE(pData->MK);

        ABC_BUF_FREE(pData->BitcoinPrivateSeed);
    }
}

/**
 * Gets information on the given wallet.
 *
 * This function allocates and fills in an wallet info structure with the information
 * associated with the given wallet UUID
 *
 * @param ppWalletInfo          Pointer to store the pointer of the allocated wallet info struct
 * @param pError                A pointer to the location to store the error if there is one
 */
tABC_CC ABC_WalletGetInfo(Wallet &self,
                          tABC_WalletInfo **ppWalletInfo,
                          tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    AutoCoreLock lock(gCoreMutex);

    tWalletData     *pData = NULL;
    tABC_WalletInfo *pInfo = NULL;

    // load the wallet data into the cache
    ABC_CHECK_RET(ABC_WalletCacheData(self, &pData, pError));

    // create the wallet info struct
    ABC_NEW(pInfo, tABC_WalletInfo);

    // copy data from what was cachqed
    ABC_STRDUP(pInfo->szUUID, self.id().c_str());
    if (pData->szName != NULL)
    {
        ABC_STRDUP(pInfo->szName, pData->szName);
    }
    pInfo->currencyNum = pData->currencyNum;
    {
        bool archived;
        ABC_CHECK_NEW(self.account.wallets.archived(archived, self.id()));
        pInfo->archived = archived;
    }
    ABC_CHECK_NEW(self.balance(pInfo->balanceSatoshi));

    // assign it to the user's pointer
    *ppWalletInfo = pInfo;
    pInfo = NULL;

exit:
    ABC_CLEAR_FREE(pInfo, sizeof(tABC_WalletInfo));

    return cc;
}

/**
 * Free the wallet info.
 *
 * This function frees the info struct returned from ABC_WalletGetInfo.
 *
 * @param pWalletInfo   Wallet info to be free'd
 */
void ABC_WalletFreeInfo(tABC_WalletInfo *pWalletInfo)
{
    if (pWalletInfo != NULL)
    {
        ABC_FREE_STR(pWalletInfo->szUUID);
        ABC_FREE_STR(pWalletInfo->szName);

        ABC_CLEAR_FREE(pWalletInfo, sizeof(sizeof(tABC_WalletInfo)));
    }
}

} // namespace abcds
