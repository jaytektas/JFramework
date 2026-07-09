#pragma once

// BaseWidgets.h — UMBRELLA. The ~30 widget classes formerly defined here now each live in
// their own J<Name>.h. This header simply re-includes them in dependency order so that
// `#include <j/core/BaseWidgets.h>` still pulls in the full widget set, unchanged, for every
// existing consumer. Add new widgets as their own header + an include line below.

#include "JWidget.h"
#include "JControl.h"
#include "JTextHelper.h"
#include "JWindowControls.h"
#include "JSeparator.h"
#include "JLabel.h"
#include "JButton.h"
#include "JToolButton.h"
#include "JToggleButton.h"
#include "JCheckBox.h"
#include "JRadioButton.h"
#include "JSlider.h"
#include "JProgressBar.h"
#include "JScrollBar.h"
#include "JLineEdit.h"
#include "JKeySequenceEdit.h"
#include "JTextArea.h"
#include "JSpinBox.h"
#include "JDoubleSpinBox.h"
#include "JPopupItem.h"
#include "JComboBox.h"
#include "JColorButton.h"
#include "JFontButton.h"
#include "JTabBar.h"
#include "JTabWidget.h"
#include "JGroupBox.h"
#include "JContainer.h"
#include "JScrollArea.h"
#include "JListView.h"
#include "JTreeView.h"
#include "JDataGrid.h"

// (No class bodies remain here — see the individual J<Name>.h headers.)
