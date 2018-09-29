struct TreeCtrl;

typedef std::function<LRESULT(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool& discardMsg)> MsgFilter;
typedef std::function<void(TreeCtrl*, NMTVGETINFOTIP*)> OnGetInfoTip;
typedef std::function<LRESULT(TreeCtrl*, NMTREEVIEWW*, bool&)> OnTreeNotify;

// function called for every item in the tree.
// returning false stops iteration
typedef std::function<bool(TVITEM*)> TreeItemVisitor;

struct TreeCtrl {
    // creation parameters. must be set before CreateTreeCtrl() call
    HWND parent = nullptr;
    RECT initialPos = {0, 0, 0, 0};
    DWORD dwStyle = 0;
    DWORD dwExStyle = 0;
    HMENU menu = nullptr;
    COLORREF bgCol = 0;
    WCHAR infotipBuf[INFOTIPSIZE + 1]; // +1 just in case

    // this data can be set directly
    MsgFilter preFilter; // called at start of windows proc to allow intercepting messages
    // when set, allows the caller to set info tip by updating NMTVGETINFOTIP
    OnGetInfoTip onGetInfoTip;
    OnTreeNotify onTreeNotify;

    // private
    HWND hwnd = nullptr;
    TVITEM item = {0};
    UINT_PTR hwndSubclassId = 0;
    UINT_PTR hwndParentSubclassId = 0;
};

/* Creation sequence:
- AllocTreeCtrl()
- set creation parameters
- CreateTreeCtrl()
*/

TreeCtrl* AllocTreeCtrl(HWND parent, RECT* initialPosition);
bool CreateTreeCtrl(TreeCtrl*, const WCHAR* title);

void ClearTreeCtrl(TreeCtrl*);
TVITEM* TreeCtrlGetItem(TreeCtrl*, HTREEITEM);
std::wstring TreeCtrlGetInfoTip(TreeCtrl*, HTREEITEM);
HTREEITEM TreeCtrlGetRoot(TreeCtrl*);
HTREEITEM TreeCtrlGetChild(TreeCtrl*, HTREEITEM);
HTREEITEM TreeCtrlGetNextSibling(TreeCtrl*, HTREEITEM);
HTREEITEM TreeCtrlGetSelection(TreeCtrl*);
bool TreeCtrlSelectItem(TreeCtrl*, HTREEITEM);
HTREEITEM TreeCtrlInsertItem(TreeCtrl*, TV_INSERTSTRUCT*);

void DeleteTreeCtrl(TreeCtrl*);

void SetFont(TreeCtrl*, HFONT);
void TreeCtrlVisitNodes(TreeCtrl*, const TreeItemVisitor& visitor);
// TODO: create 2 functions for 2 different fItemRect values
bool TreeCtrlGetItemRect(TreeCtrl*, HTREEITEM, bool fItemRect, RECT& r);

void TreeViewExpandRecursively(HWND hTree, HTREEITEM hItem, UINT flag, bool subtree);
