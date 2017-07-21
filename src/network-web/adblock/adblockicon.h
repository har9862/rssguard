// This file is part of RSS Guard.
//
// Copyright (C) 2011-2017 by Martin Rotter <rotter.martinos@gmail.com>
// Copyright (C) 2010-2014 by David Rosca <nowrep@gmail.com>
//
// RSS Guard is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// RSS Guard is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with RSS Guard. If not, see <http://www.gnu.org/licenses/>.

#ifndef ADBLOCKICON_H
#define ADBLOCKICON_H

#include "gui/clickablelabel.h"

#include "network-web/adblock/adblockrule.h"


class QMenu;
class QUrl;
class QAction;
class QTimer;

class BrowserWindow;

class AdBlockIcon : public ClickableLabel {
		Q_OBJECT

	public:
		explicit AdBlockIcon(QWidget* parent = 0);
		virtual ~AdBlockIcon();

		void popupBlocked(const QString& ruleString, const QUrl& url);
		QAction* menuAction();

	public slots:
		void setEnabled(bool enabled);
		void createMenu(QMenu* menu = 0);

	private slots:
		void showMenu(const QPoint& pos);
		void toggleCustomFilter();

		void animateIcon();
		void stopAnimation();

	private:
		QAction* m_menuAction;

		QVector<QPair<AdBlockRule*, QUrl>> m_blockedPopups;
		QTimer* m_flashTimer;

		int m_timerTicks;
		bool m_enabled;
};

#endif // ADBLOCKICON_H
