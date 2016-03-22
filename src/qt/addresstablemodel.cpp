#include "addresstablemodel.h"
#include "guiutil.h"
#include "walletmodel.h"

#include "wallet.h"
#include "base58.h"
#include "stealth.h"

#include <QFont>
#include <QColor>

const QString AddressTableModel::Send = "S";
const QString AddressTableModel::Receive = "R";

struct AddressTableEntry
{
    enum Type {
        Sending,
        Receiving
    };

    Type type;
    QString label;
    QString address;
	bool stealth;

    AddressTableEntry() {}
    AddressTableEntry(Type type, const QString &label, const QString &address):
        type(type), label(label), address(address) {}
	AddressTableEntry(Type type, const QString &label, const QString &address, const bool &stealth = false):
		type(type), label(label), address(address), stealth(stealth) {}
};

struct AddressTableEntryLessThan
{
    bool operator()(const AddressTableEntry &a, const AddressTableEntry &b) const
    {
        return a.address < b.address;
    }
    bool operator()(const AddressTableEntry &a, const QString &b) const
    {
        return a.address < b;
    }
    bool operator()(const QString &a, const AddressTableEntry &b) const
    {
        return a < b.address;
    }
};

// Private implementation
class AddressTablePriv
{
public:
    CWallet *wallet;
    QList<AddressTableEntry> cachedAddressTable;
    AddressTableModel *parent;

    AddressTablePriv(CWallet *wallet, AddressTableModel *parent):
        wallet(wallet), parent(parent) {}

    void refreshAddressTable()
    {
        cachedAddressTable.clear();
        {
            LOCK(wallet->cs_wallet);
            BOOST_FOREACH(const PAIRTYPE(CTxDestination, std::string)& item, wallet->mapAddressBook)
            {
                const CBitcoinAddress& address = item.first;
                const std::string& strName = item.second;
                bool fMine = IsMine(*wallet, address.Get());
                cachedAddressTable.append(AddressTableEntry(fMine ? AddressTableEntry::Receiving : AddressTableEntry::Sending,
                                  QString::fromStdString(strName),
                                  QString::fromStdString(address.ToString())));
            }
			
			std::set<CStealthAddress>::iterator it;
            for (it = wallet->stealthAddresses.begin(); it != wallet->stealthAddresses.end(); ++it)
            {
                bool fMine = !(it->scan_secret.size() < 1);
                cachedAddressTable.append(AddressTableEntry(fMine ? AddressTableEntry::Receiving : AddressTableEntry::Sending,
                                  QString::fromStdString(it->label),
                                  QString::fromStdString(it->Encoded()),
                                  true));
            };
			
        }
    }

    void updateEntry(const QString &address, const QString &label, bool isMine, int status)
    {
        // Find address / label in model
        QList<AddressTableEntry>::iterator lower = qLowerBound(
            cachedAddressTable.begin(), cachedAddressTable.end(), address, AddressTableEntryLessThan());
        QList<AddressTableEntry>::iterator upper = qUpperBound(
            cachedAddressTable.begin(), cachedAddressTable.end(), address, AddressTableEntryLessThan());
        int lowerIndex = (lower - cachedAddressTable.begin());
        int upperIndex = (upper - cachedAddressTable.begin());
        bool inModel = (lower != upper);
        AddressTableEntry::Type newEntryType = isMine ? AddressTableEntry::Receiving : AddressTableEntry::Sending;

        switch(status)
        {
        case CT_NEW:
            if(inModel)
            {
                OutputDebugStringF("Warning: AddressTablePriv::updateEntry: Got CT_NOW, but entry is already in model\n");
                break;
            }
            parent->beginInsertRows(QModelIndex(), lowerIndex, lowerIndex);
            cachedAddressTable.insert(lowerIndex, AddressTableEntry(newEntryType, label, address));
            parent->endInsertRows();
            break;
        case CT_UPDATED:
            if(!inModel)
            {
                OutputDebugStringF("Warning: AddressTablePriv::updateEntry: Got CT_UPDATED, but entry is not in model\n");
                break;
            }
            lower->type = newEntryType;
            lower->label = label;
            parent->emitDataChanged(lowerIndex);
            break;
        case CT_DELETED:
            if(!inModel)
            {
                OutputDebugStringF("Warning: AddressTablePriv::updateEntry: Got CT_DELETED, but entry is not in model\n");
                break;
            }
            parent->beginRemoveRows(QModelIndex(), lowerIndex, upperIndex-1);
            cachedAddressTable.erase(lower, upper);
            parent->endRemoveRows();
            break;
        }
    }

    int size()
    {
        return cachedAddressTable.size();
    }

    AddressTableEntry *index(int idx)
    {
        if(idx >= 0 && idx < cachedAddressTable.size())
        {
            return &cachedAddressTable[idx];
        }
        else
        {
            return 0;
        }
    }
};

AddressTableModel::AddressTableModel(CWallet *wallet, WalletModel *parent) :
    QAbstractTableModel(parent),walletModel(parent),wallet(wallet),priv(0)
{
    columns << tr("Label") << tr("Address");
    priv = new AddressTablePriv(wallet, this);
    priv->refreshAddressTable();
}

AddressTableModel::~AddressTableModel()
{
    delete priv;
}

int AddressTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int AddressTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return columns.length();
}

QVariant AddressTableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid())
        return QVariant();

    AddressTableEntry *rec = static_cast<AddressTableEntry*>(index.internalPointer());

    if(role == Qt::DisplayRole || role == Qt::EditRole)
    {
        switch(index.column())
        {
        case Label:
            if(rec->label.isEmpty() && role == Qt::DisplayRole)
            {
                return tr("(no label)");
            }
            else
            {
                return rec->label;
            }
        case Address:
            return rec->address;
        }
    }
    else if (role == Qt::FontRole)
    {
        QFont font;
        if(index.column() == Address)
        {
            font = GUIUtil::bitcoinAddressFont();
        }
        return font;
    }
    else if (role == TypeRole)
    {
        switch(rec->type)
        {
        case AddressTableEntry::Sending:
            return Send;
        case AddressTableEntry::Receiving:
            return Receive;
        default: break;
        }
    }
    return QVariant();
}

bool AddressTableModel::setData(const QModelIndex & index, const QVariant & value, int role)
{
    if(!index.isValid())
        return false;
    AddressTableEntry *rec = static_cast<AddressTableEntry*>(index.internalPointer());

    editStatus = OK;
	std::string strTemp, strValue;

    if(role == Qt::EditRole)
    {
        switch(index.column())
        {
        case Label:
            
			strTemp = rec->address.toStdString();
            if (IsStealthAddress(strTemp))
            {
                strValue = value.toString().toStdString();
                wallet->UpdateStealthAddress(strTemp, strValue, false);
            } else
            {
                wallet->SetAddressBookName(CBitcoinAddress(strTemp).Get(), value.toString().toStdString());
            }
            break;
			
			
        case Address:
		
			std::string sTemp = value.toString().toStdString();
            if (IsStealthAddress(sTemp))
            {
                printf("TODO\n");
                editStatus = INVALID_ADDRESS;
                return false;
            }
			
            // Refuse to set invalid address, set error status and return false
            if(!walletModel->validateAddress(value.toString()))
            {
                editStatus = INVALID_ADDRESS;
                return false;
            }
            // Double-check that we're not overwriting a receiving address
            if(rec->type == AddressTableEntry::Sending)
            {
                {
                    LOCK(wallet->cs_wallet);
                    // Remove old entry
                    wallet->DelAddressBookName(CBitcoinAddress(rec->address.toStdString()).Get());
                    // Add new entry with new address
                    wallet->SetAddressBookName(CBitcoinAddress(value.toString().toStdString()).Get(), rec->label.toStdString());
                }
            }
            break;
        }

        return true;
    }
    return false;
}

QVariant AddressTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole)
        {
            return columns[section];
        }
    }
    return QVariant();
}

Qt::ItemFlags AddressTableModel::flags(const QModelIndex & index) const
{
    if(!index.isValid())
        return 0;
    AddressTableEntry *rec = static_cast<AddressTableEntry*>(index.internalPointer());

    Qt::ItemFlags retval = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    // Can edit address and label for sending addresses,
    // and only label for receiving addresses.
    if(rec->type == AddressTableEntry::Sending ||
      (rec->type == AddressTableEntry::Receiving && index.column()==Label))
    {
        retval |= Qt::ItemIsEditable;
    }
    return retval;
}

QModelIndex AddressTableModel::index(int row, int column, const QModelIndex & parent) const
{
    Q_UNUSED(parent);
    AddressTableEntry *data = priv->index(row);
    if(data)
    {
        return createIndex(row, column, priv->index(row));
    }
    else
    {
        return QModelIndex();
    }
}

void AddressTableModel::updateEntry(const QString &address, const QString &label, bool isMine, int status)
{
    // Update address book model from Bitcoin core
    priv->updateEntry(address, label, isMine, status);
}

QString AddressTableModel::addRow(const QString &type, const QString &label, const QString &address, int addressType)
{
    std::string strLabel = label.toStdString();
    std::string strAddress = address.toStdString();

    editStatus = OK;

		if(type == Send)
    {
         if (strAddress.length() > 75)
        {
            CStealthAddress sxAddr;
            if (!sxAddr.SetEncoded(strAddress))
            {
                editStatus = INVALID_ADDRESS;
                return QString();
            }
            
            // -- Check for duplicate addresses
            {
                LOCK(wallet->cs_wallet);
                
                if (wallet->stealthAddresses.count(sxAddr))
                {
                    editStatus = DUPLICATE_ADDRESS;
                    return QString();
                };
                
                sxAddr.label = strLabel;
                wallet->AddStealthAddress(sxAddr);
            }
            
        } else
        {
            if (!walletModel->validateAddress(address))
            {
                editStatus = INVALID_ADDRESS;
                return QString();
			};
            // Check for duplicate addresses
            {
                LOCK(wallet->cs_wallet);
                if (wallet->mapAddressBook.count(CBitcoinAddress(strAddress).Get()))
                {
                   editStatus = DUPLICATE_ADDRESS;
                   return QString();
                };
                
                wallet->SetAddressBookName(CBitcoinAddress(strAddress).Get(), strLabel);
            }
        }
    }
    else if(type == Receive)
    {
        // Generate a new address to associate with given label
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());
		
        if(!ctx.isValid())
        {
            // Unlock wallet failed or was cancelled
            editStatus = WALLET_UNLOCK_FAILURE;
            return QString();
        }
        if (addressType == AT_Stealth)
        {
            CStealthAddress newStealthAddr;
            std::string sError;
            if (!wallet->NewStealthAddress(sError, strLabel, newStealthAddr)
                || !wallet->AddStealthAddress(newStealthAddr))
            {
                editStatus = KEY_GENERATION_FAILURE;
                return QString();
            }
            strAddress = newStealthAddr.Encoded();
        } else
        {
            CPubKey newKey;
            if(!wallet->GetKeyFromPool(newKey, true))
            {
                editStatus = KEY_GENERATION_FAILURE;
                return QString();
            }
            strAddress = CBitcoinAddress(newKey.GetID()).ToString();
            
            {
                LOCK(wallet->cs_wallet);
                wallet->SetAddressBookName(CBitcoinAddress(strAddress).Get(), strLabel);
            }
        }
    }
    else
    {
        return QString();
    }
    
    return QString::fromStdString(strAddress);

}

/* Look up label for address in address book, if not found return empty string.
 */
QString AddressTableModel::labelForAddress(const QString &address) const
{
    {
        LOCK(wallet->cs_wallet);
        std::string sAddr = address.toStdString();
        
        if (sAddr.length() > 75)
        {
            CStealthAddress sxAddr;
            if (!sxAddr.SetEncoded(sAddr))
                return QString();
            
            std::set<CStealthAddress>::iterator it;
            it = wallet->stealthAddresses.find(sxAddr);
            if (it == wallet->stealthAddresses.end())
                return QString();
            
            return QString::fromStdString(it->label);
        } else
        {
            CBitcoinAddress address_parsed(sAddr);
            std::map<CTxDestination, std::string>::iterator mi = wallet->mapAddressBook.find(address_parsed.Get());
            if (mi != wallet->mapAddressBook.end())
            {
                return QString::fromStdString(mi->second);
            }
        }
    }
    return QString();
}

int AddressTableModel::lookupAddress(const QString &address) const
{
    QModelIndexList lst = match(index(0, Address, QModelIndex()),
                                Qt::EditRole, address, 1, Qt::MatchExactly);
    if(lst.isEmpty())
    {
        return -1;
    }
    else
    {
        return lst.at(0).row();
    }
}

void AddressTableModel::emitDataChanged(int idx)
{
    emit dataChanged(index(idx, 0, QModelIndex()), index(idx, columns.length()-1, QModelIndex()));
}
